#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace uniorm {

using timestamp = std::chrono::system_clock::time_point;

// Dynamic value type used by row and params.
using sql_value = std::variant<std::monostate,  // NULL
  bool, std::int16_t, std::int32_t, std::int64_t, double,
  std::string,  // UTF-8
  std::vector<std::byte>, timestamp>;

inline bool is_null(sql_value const& v) noexcept {
  return std::holds_alternative<std::monostate>(v);
}

}  // namespace uniorm
