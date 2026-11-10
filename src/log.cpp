#include <uniorm/log.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

namespace uniorm {
namespace {

// Function-local statics, so the sink is still reachable from a static
// destructor: a transaction's cleanup logs, and that can run once the code
// that installed the sink is long gone.
std::mutex& sink_lock() {
  static std::mutex lock;
  return lock;
}

std::shared_ptr<log_sink>& sink_slot() {
  static std::shared_ptr<log_sink> slot;
  return slot;
}

// The gate a call site reads. Both are read relaxed: being a record late by
// one store is a record the caller was about to get anyway.
std::atomic<bool> sink_present{ false };
std::atomic<int> threshold{ static_cast<int>(log_level::info) };

}  // namespace

namespace detail {

std::string current_exception_text() {
  std::exception_ptr in_flight = std::current_exception();
  if (!in_flight) {
    return "(no failure was in flight)";
  }
  try {
    std::rethrow_exception(in_flight);
  } catch (std::exception const& error) {
    return error.what();
  } catch (...) {
    return "(a failure that carries no words)";
  }
}

}  // namespace detail

void set_log_sink(log_sink sink) {
  bool const armed = static_cast<bool>(sink);
  std::lock_guard lock(sink_lock());
  sink_slot() = armed ? std::make_shared<log_sink>(std::move(sink)) : nullptr;
  sink_present.store(armed, std::memory_order_relaxed);
}

void set_log_level(log_level wanted) {
  threshold.store(static_cast<int>(wanted), std::memory_order_relaxed);
}

log_level log_level_threshold() {
  return static_cast<log_level>(threshold.load(std::memory_order_relaxed));
}

bool log_enabled(log_level level) {
  return sink_present.load(std::memory_order_relaxed) &&
         static_cast<int>(level) <= threshold.load(std::memory_order_relaxed);
}

namespace detail {

void log(log_level level, std::string_view site, std::string_view message) {
  if (!log_enabled(level)) {
    return;
  }
  std::shared_ptr<log_sink> current;
  {
    std::lock_guard lock(sink_lock());
    current = sink_slot();
  }
  // Held across the call, so a sink replaced on another thread is not torn
  // down under the one still running, and this record goes to a whole sink.
  if (!current) {
    return;
  }
  try {
    (*current)(log_record{ level, site, message });
  } catch (...) {
    // The failure this record was written about outranks whatever the sink
    // did about it; nothing here is allowed to replace the first.
  }
}

}  // namespace detail
}  // namespace uniorm
