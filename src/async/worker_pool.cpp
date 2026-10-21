#include "worker_pool.hpp"

#include <memory>
#include <utility>

#include <uniorm/error.hpp>

namespace uniorm::async {
namespace {

std::size_t hardware_workers() {
  unsigned const found = std::thread::hardware_concurrency();
  return found > 0 ? std::size_t(found) : 4;
}

// The default pool appears on first use, sized by whatever
// default_executor_size() was told before then -- so a caller naming a count
// at start-up is not fighting the machine's core count.
struct pool_bootstrap {
  std::mutex mutex;
  std::size_t intended = 0;
  std::unique_ptr<worker_pool> pool;

  worker_pool& use() {
    std::lock_guard lock(mutex);
    if (!pool) {
      std::size_t const threads =
        intended > 0 ? intended : hardware_workers();
      pool = std::make_unique<worker_pool>(threads);
    }
    return *pool;
  }
};

pool_bootstrap& bootstrap() {
  static pool_bootstrap state;
  return state;
}

}  // namespace

worker_pool::worker_pool(std::size_t threads) {
  thread_count(threads);
}

worker_pool::~worker_pool() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  work_ready_.notify_all();
  for (auto& worker : workers_) {
    worker.join();
  }
}

void worker_pool::submit(std::function<void()> work) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      throw uniorm_error("worker pool: shutting down, work refused");
    }
    queue_.push_back(std::move(work));
  }
  work_ready_.notify_one();
}

void worker_pool::thread_count(std::size_t threads) {
  std::lock_guard lock(mutex_);
  if (threads == 0) {
    throw uniorm_error("worker pool: needs at least one worker");
  }
  if (threads < workers_.size()) {
    throw uniorm_error("worker pool: the worker count is never lowered");
  }
  while (workers_.size() < threads) {
    workers_.emplace_back([this] { run(); });
  }
}

std::size_t worker_pool::thread_count() const {
  std::lock_guard lock(mutex_);
  return workers_.size();
}

void worker_pool::run() {
  for (;;) {
    std::function<void()> work;
    {
      std::unique_lock lock(mutex_);
      work_ready_.wait(lock,
        [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        return;
      }
      work = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      work();
    } catch (...) {
      // Whatever a closure throws belongs to the operation it runs, not to
      // this thread: letting it escape takes the worker, the pool, and every
      // session that was waiting on it down with it.
    }
  }
}

executor& default_executor() {
  return bootstrap().use();
}

void default_executor_size(std::size_t threads) {
  auto& state = bootstrap();
  std::lock_guard lock(state.mutex);
  if (state.pool) {
    // Already running: it can grow, and a worker deep in a driver call is not
    // coming out of it.
    state.pool->thread_count(threads);
    return;
  }
  if (threads == 0) {
    throw uniorm_error("worker pool: needs at least one worker");
  }
  state.intended = threads;
}

}  // namespace uniorm::async
