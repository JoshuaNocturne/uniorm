#include "async_executors.hpp"
#include "check.hpp"

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>

#include <uniorm/async.hpp>
#include <uniorm/error.hpp>

namespace {

using uniorm::async::task;
using uniorm::test::hand_pump;
using uniorm::test::same_thread;

task<int> counted(std::size_t& runs, int answer) {
  ++runs;
  co_return answer;
}

task<void> counted_void(std::size_t& runs) {
  ++runs;
  co_return;
}

// The throw sits inside the co_return's own expression so the body is a
// coroutine by construction: one naming no coroutine keyword is an ordinary
// function, and its throw would sail past the promise handler.
int explode() {
  throw std::runtime_error("body failed");
}

task<int> exploding(std::size_t& runs) {
  ++runs;
  co_return explode();
}

// The two awaits hold no worker: with one worker in the pool, an outer body
// that kept its thread while waiting could never see its inner task run.
task<int> awaiting_twice(std::size_t& runs) {
  int head = co_await counted(runs, 20);
  int tail = co_await counted(runs, 22);
  co_return head + tail;
}

task<int> awaiting_failure(std::size_t& runs) {
  co_return co_await exploding(runs);
}

void test_making_a_task_runs_nothing() {
  std::size_t runs = 0;
  hand_pump pump;
  {
    auto work = counted(runs, 5);
    CHECK(runs == 0);
    CHECK(pump.queued() == 0);
  }
  // Dropping an undriven task is legal and leaves nothing behind: no body, no
  // queued closure.
  CHECK(runs == 0);
  CHECK(pump.queued() == 0);
}

void test_block_parks_and_returns() {
  std::size_t runs = 0;
  same_thread pump;
  auto work = counted(runs, 42);
  CHECK(work.block(pump) == 42);
  CHECK(runs == 1);
  CHECK(pump.submissions() == 1);

  auto nothing = counted_void(runs);
  nothing.block(pump);
  CHECK(runs == 2);
}

void test_block_rethrows_the_body() {
  std::size_t runs = 0;
  same_thread pump;
  auto work = exploding(runs);
  std::string seen;
  try {
    work.block(pump);
  } catch (std::runtime_error const& error) {
    seen = error.what();
  }
  CHECK(seen == "body failed");
  CHECK(runs == 1);
}

// The delivered failure is the body's own object: two rethrows of one pointer
// land on one address, so nothing re-wrapped it on the way out.
void test_failure_reaches_the_handler_unchanged() {
  std::size_t runs = 0;
  same_thread pump;
  std::exception_ptr carried;
  auto work = exploding(runs);
  work.start(pump, [&](std::exception_ptr failure) { carried = failure; });
  CHECK(static_cast<bool>(carried));

  std::runtime_error const* first = nullptr;
  std::string seen;
  try {
    std::rethrow_exception(carried);
  } catch (std::runtime_error const& error) {
    first = &error;
    seen = error.what();
  }
  CHECK(seen == "body failed");
  try {
    std::rethrow_exception(carried);
  } catch (std::runtime_error const& error) {
    CHECK(&error == first);
  }
  CHECK(runs == 1);
}

void test_start_reports_only_a_failure() {
  std::size_t runs = 0;
  std::size_t reported = 0;
  hand_pump pump;
  {
    auto ok = counted(runs, 7);
    ok.start(pump, [&](std::exception_ptr failure) {
      if (failure) {
        ++reported;
      }
    });
    auto bad = exploding(runs);
    bad.start(pump, [&](std::exception_ptr failure) {
      if (failure) {
        ++reported;
      }
    });
    CHECK(runs == 0);
    CHECK(pump.queued() == 2);
  }
  // Both task objects are gone; the work they handed over still runs, and only
  // the failing one reaches a handler.
  pump.run_all();
  CHECK(runs == 2);
  CHECK(reported == 1);
}

void test_await_delivers_through_the_chain() {
  // Called before any task has used the default pool, so this is the count the
  // pool starts with rather than a request to shrink it.
  uniorm::async::default_executor_size(1);
  std::size_t runs = 0;
  auto chain = awaiting_twice(runs);
  CHECK(chain.block() == 42);
  CHECK(runs == 2);

  std::size_t failed_runs = 0;
  auto failed = awaiting_failure(failed_runs);
  CHECK_THROWS(failed.block(), std::runtime_error);
  CHECK(failed_runs == 1);
}

// One pool serves the whole process, and a count of zero would leave it with
// nowhere to run a body: refused both before the pool exists and after.
void test_the_default_pool_refuses_a_count_of_zero() {
  CHECK_THROWS(uniorm::async::default_executor_size(0), uniorm::uniorm_error);
  CHECK_THROWS(uniorm::async::default_executor_size(0), uniorm::uniorm_error);
}

void test_driving_twice_is_refused() {
  std::size_t runs = 0;
  same_thread pump;
  auto work = counted(runs, 3);
  CHECK(work.block(pump) == 3);
  CHECK_THROWS(work.block(pump), uniorm::uniorm_error);
  CHECK_THROWS(work.start(pump, nullptr), uniorm::uniorm_error);
  CHECK(runs == 1);
}

}  // namespace

void test_async_task() {
  test_making_a_task_runs_nothing();
  test_block_parks_and_returns();
  test_block_rethrows_the_body();
  test_failure_reaches_the_handler_unchanged();
  test_start_reports_only_a_failure();
  test_await_delivers_through_the_chain();
  test_the_default_pool_refuses_a_count_of_zero();
  test_driving_twice_is_refused();
}
