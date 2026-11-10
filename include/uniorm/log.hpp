#pragma once

// Public header: the log sink the host application installs, and the records
// the library hands it. Nothing is logged until a sink is set. A record
// carries the library's own words about what it did, never the data it moved:
// no bound value and no parameter reaches it.

#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <uniorm/export.hpp>

namespace uniorm {

// Ordered, most urgent first. `error` marks a failure the caller's own
// operation died at, `warn` a failure the library absorbed and worked around,
// `info` a state change worth seeing in production, `debug` the mechanics of
// one statement, `trace` the work of one row or column.
enum class log_level { error, warn, info, debug, trace };

// One record. `site` names the part of the library that spoke, in
// "<area>.<part>" form, and is what a sink filters or prefixes on; `message`
// is a sentence of human-readable text.
struct log_record {
  log_level level;
  std::string_view site;
  std::string_view message;
};

using log_sink = std::function<void(log_record const&)>;

// Replaces the sink, or stops the logging when handed an empty one, which is
// the state the library starts in. A sink is reached from whichever thread the
// work happened on, and from a destructor, so it has to hold up under being
// called concurrently; an exception out of it is dropped.
UNIORM_API void set_log_sink(log_sink sink);

// Records below this level are neither built nor handed over. The threshold
// starts at `info`.
UNIORM_API void set_log_level(log_level threshold);
UNIORM_API log_level log_level_threshold();

// Whether a record at this level would reach a sink. Answering costs one
// relaxed load, so a call site that has to assemble a message asks first.
UNIORM_API bool log_enabled(log_level level);

namespace detail {

// The emit point, in two spellings. A message already in words goes in as it
// is; a call site that has to assemble one hands the assembly over, and it
// runs only once the level has passed the gate. Both ask `log_enabled`
// themselves, so no call site needs a guard of its own.
UNIORM_API void log(
  log_level level, std::string_view site, std::string_view message);

template <class Maker>
  requires std::invocable<Maker&> &&
           std::convertible_to<std::invoke_result_t<Maker&>, std::string>
inline void log(log_level level, std::string_view site, Maker&& make) {
  if (log_enabled(level)) {
    log(level, site, std::forward<Maker>(make)());
  }
}

// The words of the exception in flight, for a record about a failure the
// library chose not to rethrow. Only meaningful inside a handler.
UNIORM_API std::string current_exception_text();

}  // namespace detail
}  // namespace uniorm
