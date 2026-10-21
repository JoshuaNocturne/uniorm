#pragma once

// The thread pool behind default_executor(). Its queue answers a different
// question from a session's: this one is "which thread is free", that one is
// "who goes next on this connection".

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <uniorm/async.hpp>

namespace uniorm::async {

class worker_pool final : public executor {
public:
  explicit worker_pool(std::size_t threads);
  ~worker_pool() override;

  worker_pool(worker_pool const&) = delete;
  worker_pool& operator=(worker_pool const&) = delete;

  void submit(std::function<void()> work) override;

  // Grow only: a worker is inside a driver call for the whole length of an
  // operation, and nothing can lift it out of one.
  void thread_count(std::size_t threads);
  std::size_t thread_count() const;

private:
  void run();

  mutable std::mutex mutex_;
  std::condition_variable work_ready_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

}  // namespace uniorm::async
