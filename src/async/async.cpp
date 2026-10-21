#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <uniorm/async.hpp>
#include <uniorm/error.hpp>

namespace uniorm::async {

// Out of line so this interface's vtable lands in the library: a caller who
// derives from executor in their own translation unit reaches it by link, not
// by a copy emitted beside their loop.
executor::~executor() = default;

namespace detail {

std::size_t operation_queue::submit_limit() const noexcept {
  return limit_.load(std::memory_order_relaxed);
}

void operation_queue::submit_limit(std::size_t count) noexcept {
  limit_.store(count == 0 ? 1 : count, std::memory_order_relaxed);
}

std::size_t operation_queue::pending() const {
  std::scoped_lock lock(mutex_);
  return waiting_.size() + (in_flight_ ? 1 : 0);
}

void operation_queue::push(starter start) {
  std::unique_lock lock(mutex_);
  if (waiting_.size() + (in_flight_ ? 1 : 0) >=
    limit_.load(std::memory_order_relaxed)) {
    throw submit_overflow(
      "async session: submit_limit reached, nothing started");
  }
  waiting_.push_back(std::move(start));
  if (!in_flight_ && !draining_) {
    drain(lock);
  }
}

void operation_queue::finish() noexcept {
  std::unique_lock lock(mutex_);
  in_flight_ = false;
  if (!draining_) {
    drain(lock);
  }
  if (!in_flight_ && waiting_.empty()) {
    idle_.notify_all();
  }
}

void operation_queue::wait_idle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return waiting_.empty() && !in_flight_; });
}

void operation_queue::drain(std::unique_lock<std::mutex>& lock) {
  draining_ = true;
  while (!waiting_.empty()) {
    auto start = std::move(waiting_.front());
    waiting_.pop_front();
    in_flight_ = true;
    lock.unlock();
    try {
      start();
    } catch (...) {
      // A starter that throws has answered its own operation already, and no
      // finish() is coming for this turn: free the slot and walk on, or the
      // queue never starts anything again.
      lock.lock();
      in_flight_ = false;
      if (waiting_.empty()) {
        idle_.notify_all();
      }
      continue;
    }
    lock.lock();
    if (in_flight_) {
      // Still running somewhere else; that finish() walks on from here.
      break;
    }
  }
  draining_ = false;
}

}  // namespace detail

orm::orm() = default;

orm::orm(std::string_view connection_string) : db_(connection_string) {}

orm::orm(connection_spec const& spec) : db_(spec) {}

orm::orm(connection_pool& pool) : db_(pool) {}

orm::orm(uniorm::orm db) : db_(std::move(db)) {}

std::size_t orm::row_array_size() const noexcept {
  return db_.row_array_size();
}

void orm::row_array_size(std::size_t size) noexcept {
  db_.row_array_size(size);
}

std::size_t orm::paramset_size() const noexcept {
  return db_.paramset_size();
}

void orm::paramset_size(std::size_t size) noexcept {
  db_.paramset_size(size);
}

bool orm::auto_commit() const noexcept {
  return db_.auto_commit();
}

dialect::identifier_case orm::identifier_case() const noexcept {
  return db_.identifier_case();
}

void orm::identifier_case(dialect::identifier_case policy) {
  db_.identifier_case(policy);
}

uniorm::orm& orm::sync() noexcept {
  return db_;
}

void orm::run_on(executor& target) noexcept {
  run_on_ = &target;
}

void orm::resume_on(executor& target) noexcept {
  resume_on_ = &target;
}

std::size_t orm::submit_limit() const noexcept {
  return queue_.submit_limit();
}

void orm::submit_limit(std::size_t count) noexcept {
  queue_.submit_limit(count);
}

std::size_t orm::pending() const {
  return queue_.pending();
}

// Whatever this session handed over finishes before it goes: a queued closure
// points back here, and the connection it uses is this object's. An open
// transaction handle outliving the session is the same hazard by another shape,
// so the handles it handed out are closed here too, while the connection that
// has to serve the rollback is still alive.
orm::~orm() {
  queue_.wait_idle();
  door_->open = false;
  for (auto& entry : open_transactions_) {
    if (auto open = entry.lock()) {
      try {
        open->rollback();
      } catch (...) {
        // The connection is going either way; a driver that refuses the
        // rollback now has it on its own conscience.
      }
    }
  }
}

void orm::wait_all_operations() {
  queue_.wait_idle();
}

task<void> orm::connect(std::string_view connection_string) {
  return enqueue<void>(
    [this, text = std::string(connection_string)] { db_.connect(text); });
}

task<void> orm::connect(connection_spec const& spec) {
  return enqueue<void>([this, copy = spec] { db_.connect(copy); });
}

task<void> orm::disconnect() {
  return enqueue<void>([this] { db_.disconnect(); });
}

task<std::size_t> orm::execute_update(
  std::string_view sql, params const& bound) {
  return enqueue<std::size_t>([this, text = std::string(sql), copy = bound] {
    return db_.execute_update(text, copy);
  });
}

task<void> orm::auto_commit(bool enabled) {
  return enqueue<void>([this, enabled] { db_.auto_commit(enabled); });
}

task<void> orm::commit() {
  return enqueue<void>([this] { db_.commit(); });
}

task<void> orm::rollback() {
  return enqueue<void>([this] { db_.rollback(); });
}

task<transaction> orm::begin() {
  return enqueue<transaction>([this] {
    auto opened = std::make_shared<uniorm::transaction>(db_.begin());
    return transaction(std::move(opened), *this);
  });
}

bool transaction::active() const noexcept {
  return inner_ && inner_->active();
}

transaction::transaction(std::shared_ptr<uniorm::transaction> inner,
  orm& session)
  : inner_(std::move(inner)), session_(&session), door_(session.door_) {
  // Handing the session this handle's transaction is what makes the promise in
  // the header hold: the registry is weak, so closing a handle stays the
  // handle's own business, and only the ones still open are left for ~orm().
  auto& registry = session.open_transactions_;
  registry.erase(
    std::remove_if(registry.begin(), registry.end(),
      [](std::weak_ptr<uniorm::transaction> const& entry) {
        return entry.expired();
      }),
    registry.end());
  registry.push_back(inner_);
}

task<void> transaction::commit() {
  if (!door_ || !door_->open) {
    throw uniorm_error(
      "async transaction: the session behind this handle is gone");
  }
  return session_->enqueue<void>([inner = inner_] { inner->commit(); });
}

task<void> transaction::rollback() {
  if (!door_ || !door_->open) {
    throw uniorm_error(
      "async transaction: the session behind this handle is gone");
  }
  return session_->enqueue<void>([inner = inner_] { inner->rollback(); });
}

transaction::~transaction() {
  if (!active() || !door_ || !door_->open) {
    return;
  }
  try {
    session_->queue_.wait_idle();
    inner_->rollback();
  } catch (...) {
    // destruction must not throw: the lease going back to the pool is the net
    // under whatever this one could not close.
  }
}

}  // namespace uniorm::async
