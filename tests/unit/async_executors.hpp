#pragma once

// The two ways a test decides when a task's body runs: on the spot, or when the
// test says so. Both are the mechanism's own seams -- production callers hand
// over a loop, and a session's run_on() overrides them.

#include <cstddef>
#include <deque>
#include <functional>
#include <utility>

#include <uniorm/async.hpp>

namespace uniorm::test {

// Runs the work before submit() returns, so a drive action is over the moment
// it is taken.
class same_thread final : public uniorm::async::executor {
public:
  void submit(std::function<void()> work) override {
    ++submissions_;
    work();
  }

  std::size_t submissions() const noexcept {
    return submissions_;
  }

private:
  std::size_t submissions_ = 0;
};

// Holds the work until the test chooses the moment it runs.
class hand_pump final : public uniorm::async::executor {
public:
  void submit(std::function<void()> work) override {
    queue_.push_back(std::move(work));
  }

  std::size_t queued() const noexcept {
    return queue_.size();
  }

  // One submitted body, or nothing if the queue is empty.
  std::size_t run_one() {
    if (queue_.empty()) {
      return 0;
    }
    auto work = std::move(queue_.front());
    queue_.pop_front();
    work();
    return 1;
  }

  // Drains including anything a running body queues on the way.
  std::size_t run_all() {
    std::size_t run = 0;
    while (run_one() != 0) {
      ++run;
    }
    return run;
  }

private:
  std::deque<std::function<void()>> queue_;
};

}  // namespace uniorm::test
