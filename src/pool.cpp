#include <uniorm/pool.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace uniorm {

struct pool_detail::shared_state {
  struct idle_entry {
    connection conn;
    std::chrono::steady_clock::time_point released_at;
  };

  pool_options options;
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<idle_entry> idle;
  std::size_t created = 0;
  std::atomic<unsigned long long> heartbeats{ 0 };
  bool maintained = false;  // registered with the global scheduler
  std::chrono::steady_clock::time_point next_tick;

  bool expired(idle_entry const& e) const {
    return std::chrono::steady_clock::now() - e.released_at >
           options.max_idle_time;
  }

  // One maintenance pass: drop idle-expired connections, heartbeat the rest.
  // Called by the global scheduler thread; uses only this pool's own mutex.
  void run_maintenance() {
    std::unique_lock lock(mutex);
    std::vector<idle_entry> evicted;
    for (auto it = idle.begin(); it != idle.end();) {
      if (expired(*it)) {
        evicted.push_back(std::move(*it));
        it = idle.erase(it);
        --created;
      } else {
        ++it;
      }
    }
    // take the rest out so heartbeats run without holding the lock
    // (a heartbeat must not observe the pool mutex, e.g. via idle_count())
    std::vector<idle_entry> checking = std::move(idle);
    idle.clear();
    lock.unlock();

    evicted.clear();  // disconnect outside the lock: can block on the server

    std::vector<idle_entry> alive;
    for (auto& e : checking) {
      try {
        e.conn.execute(options.heartbeat_sql);
        ++heartbeats;
        alive.push_back(std::move(e));
      } catch (...) {
        // heartbeat failed: connection is considered dead and dropped
      }
    }

    lock.lock();
    std::size_t dead = checking.size() - alive.size();
    created -= dead;
    for (auto& e : alive) {
      idle.push_back(std::move(e));
    }
    if (dead > 0) {
      cv.notify_all();  // slots freed; waiters may create replacements
    }
  }
};

namespace {

// One background thread servicing every connection_pool that opted into
// heartbeats. Pools are tracked as weak_ptr so a destroyed pool is simply
// skipped; locking the weak_ptr keeps the impl alive during a maintenance
// pass. The scheduler is constructed on first use and outlives every pool
// (function-local static, created during the first pool's constructor).
struct scheduler {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::weak_ptr<pool_detail::shared_state>> pools;
  std::thread worker;
  bool running = false;
  bool stop = false;

  static scheduler& instance() {
    static scheduler s;
    return s;
  }

  void add(std::shared_ptr<pool_detail::shared_state> const& p) {
    p->next_tick =
      std::chrono::steady_clock::now() + p->options.heartbeat_interval;
    bool need_start;
    {
      std::lock_guard lock(mutex);
      pools.push_back(p);
      need_start = !running && !stop;
      if (need_start) {
        running = true;
      }
    }
    cv.notify_all();
    if (need_start) {
      if (worker.joinable()) {
        worker.join();  // previous run already exited (registry was empty)
      }
      worker = std::thread([this] { run(); });
    }
  }

  void remove(std::shared_ptr<pool_detail::shared_state> const& p) {
    {
      std::lock_guard lock(mutex);
      pools.erase(std::remove_if(pools.begin(), pools.end(),
                    [&](std::weak_ptr<pool_detail::shared_state> const& wp) {
                      return !wp.owner_before(p) && !p.owner_before(wp);
                    }),
        pools.end());
    }
    cv.notify_all();  // let the worker notice an emptied registry sooner
  }

  void run() {
    std::unique_lock lock(mutex);
    while (!stop) {
      pools.erase(std::remove_if(pools.begin(), pools.end(),
                    [](std::weak_ptr<pool_detail::shared_state> const& wp) {
                      return wp.expired();
                    }),
        pools.end());
      if (pools.empty()) {
        running = false;
        break;
      }

      auto now = std::chrono::steady_clock::now();
      auto earliest = std::chrono::steady_clock::time_point::max();
      for (auto& wp : pools) {
        if (auto sp = wp.lock()) {
          if (sp->next_tick < earliest) {
            earliest = sp->next_tick;
          }
        }
      }
      if (earliest > now) {
        cv.wait_until(lock, earliest);
        continue;
      }

      std::vector<std::shared_ptr<pool_detail::shared_state>> due;
      for (auto& wp : pools) {
        if (auto sp = wp.lock()) {
          if (sp->next_tick <= now) {
            due.push_back(std::move(sp));
          }
        }
      }
      lock.unlock();

      for (auto& sp : due) {
        sp->run_maintenance();
      }
      auto next = std::chrono::steady_clock::now();
      for (auto& sp : due) {
        sp->next_tick = next + sp->options.heartbeat_interval;
      }

      lock.lock();
    }
  }

  ~scheduler() {
    {
      std::lock_guard lock(mutex);
      stop = true;
    }
    cv.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }
};

}  // namespace

connection_pool::connection_pool(pool_options options)
  : impl_(std::make_shared<pool_detail::shared_state>()) {
  impl_->options = std::move(options);
  if (impl_->options.heartbeat_interval > std::chrono::milliseconds::zero()) {
    impl_->maintained = true;
    scheduler::instance().add(impl_);
  }
}

connection_pool::~connection_pool() {
  if (impl_->maintained) {
    scheduler::instance().remove(impl_);
  }
  // checked-out connections still alive will disconnect on their own; the
  // pool must outlive them per the documented contract
}

pooled_connection connection_pool::acquire() {
  std::unique_lock lock(impl_->mutex);
  auto deadline =
    std::chrono::steady_clock::now() + impl_->options.acquire_timeout;
  while (true) {
    if (!impl_->idle.empty()) {
      pool_detail::shared_state::idle_entry e = std::move(impl_->idle.back());
      impl_->idle.pop_back();
      if (!impl_->expired(e)) {
        lock.unlock();
        return pooled_connection(this, std::move(e.conn));
      }
      --impl_->created;
      lock.unlock();
      impl_->cv.notify_one();  // slot freed; a waiter may create a fresh one
      continue;  // e is destroyed here (connection closed)
    }
    if (impl_->created < impl_->options.size) {
      ++impl_->created;
      lock.unlock();
      try {
        connection conn(impl_->options.connection_string);
        return pooled_connection(this, std::move(conn));
      } catch (...) {
        {
          std::lock_guard relock(impl_->mutex);
          --impl_->created;
        }
        impl_->cv.notify_one();
        throw;
      }
    }
    if (impl_->cv.wait_until(lock, deadline) == std::cv_status::timeout &&
        impl_->idle.empty()) {
      throw pool_timeout(
        "no connection became available within acquire_timeout");
    }
  }
}

std::size_t connection_pool::capacity() const {
  return impl_->options.size;
}

std::size_t connection_pool::idle_count() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->idle.size();
}

unsigned long long connection_pool::heartbeats_executed() const {
  return impl_->heartbeats.load();
}

void connection_pool::release(connection conn) {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->idle.push_back(
      { std::move(conn), std::chrono::steady_clock::now() });
  }
  impl_->cv.notify_one();
}

pooled_connection::pooled_connection(connection_pool* pool, connection conn)
  : pool_(pool), conn_(std::move(conn)), valid_(true) {}

pooled_connection::~pooled_connection() {
  if (valid_ && pool_) {
    pool_->release(std::move(conn_));
  }
}

pooled_connection::pooled_connection(pooled_connection&& other) noexcept
  : pool_(other.pool_), conn_(std::move(other.conn_)), valid_(other.valid_) {
  other.pool_ = nullptr;
  other.valid_ = false;
}

pooled_connection& pooled_connection::operator=(
  pooled_connection&& other) noexcept {
  if (this != &other) {
    if (valid_ && pool_) {
      pool_->release(std::move(conn_));
    }
    pool_ = other.pool_;
    conn_ = std::move(other.conn_);
    valid_ = other.valid_;
    other.pool_ = nullptr;
    other.valid_ = false;
  }
  return *this;
}

bool pooled_connection::is_open() const noexcept {
  return valid_ && conn_.is_open();
}

// --- connection_pool_registry ---

namespace {

std::string to_lower(std::string_view s) {
  std::string result(s);
  std::transform(result.begin(), result.end(), result.begin(),
    [](unsigned char c) { return std::tolower(c); });
  return result;
}

std::string trim(std::string_view s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) return "";
  auto end = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(start, end - start + 1));
}

}  // namespace

std::string connection_pool_registry::make_key(
  std::string const& connection_string) {
  std::string dsn, uid;

  std::string_view sv(connection_string);
  std::size_t pos = 0;
  while (pos < sv.size()) {
    auto semi = sv.find(';', pos);
    auto token = (semi == std::string_view::npos) ? sv.substr(pos)
                                                   : sv.substr(pos, semi - pos);
    auto eq = token.find('=');
    if (eq != std::string_view::npos) {
      auto key = to_lower(trim(token.substr(0, eq)));
      auto value = trim(token.substr(eq + 1));
      if (key == "dsn") dsn = value;
      else if (key == "uid" || key == "user" || key == "username")
        uid = value;
    }
    pos = (semi == std::string_view::npos) ? sv.size() : semi + 1;
  }

  return "dsn=" + dsn + "|uid=" + uid;
}

connection_pool_registry& connection_pool_registry::instance() {
  static connection_pool_registry reg;
  return reg;
}

pooled_connection connection_pool_registry::acquire(
  std::string const& connection_string) {
  std::string key = make_key(connection_string);

  std::lock_guard lock(mutex_);
  auto it = pools_.find(key);
  if (it == pools_.end()) {
    pool_options opts;
    opts.connection_string = connection_string;
    auto [new_it, _] = pools_.emplace(
      key, std::make_unique<connection_pool>(std::move(opts)));
    it = new_it;
  }
  return it->second->acquire();
}

void connection_pool_registry::configure(
  std::string const& connection_string, pool_options opts) {
  std::string key = make_key(connection_string);
  opts.connection_string = connection_string;

  std::lock_guard lock(mutex_);
  pools_.emplace(key, std::make_unique<connection_pool>(std::move(opts)));
}

}  // namespace uniorm
