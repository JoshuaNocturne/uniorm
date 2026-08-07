#pragma once

#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "detail/param_staging.hpp"
#include "error.hpp"
#include "export.hpp"
#include "value.hpp"

namespace uniorm {

namespace odbc {
class statement;
}

namespace detail {

template <class T>
sql_value make_sql_value(T&& v) {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<U, std::nullptr_t>) {
    return std::monostate{};
  } else if constexpr (std::is_same_v<U, std::monostate> ||
                       std::is_same_v<U, bool> ||
                       std::is_same_v<U, std::int16_t> ||
                       std::is_same_v<U, std::int32_t> ||
                       std::is_same_v<U, std::int64_t> ||
                       std::is_same_v<U, double> ||
                       std::is_same_v<U, std::string> ||
                       std::is_same_v<U, std::vector<std::byte>> ||
                       std::is_same_v<U, timestamp>) {
    return sql_value(std::forward<T>(v));
  } else if constexpr (std::is_integral_v<U>) {
    if constexpr (sizeof(U) <= sizeof(std::int32_t)) {
      if constexpr (std::is_signed_v<U>) {
        return sql_value(static_cast<std::int32_t>(v));
      } else {
        return sql_value(static_cast<std::int64_t>(v));
      }
    } else if constexpr (std::is_signed_v<U>) {
      return sql_value(static_cast<std::int64_t>(v));
    } else {
      if (v > static_cast<U>(std::numeric_limits<std::int64_t>::max())) {
        throw type_mismatch("unsigned value exceeds int64 range");
      }
      return sql_value(static_cast<std::int64_t>(v));
    }
  } else if constexpr (std::is_floating_point_v<U>) {
    return sql_value(static_cast<double>(v));
  } else if constexpr (std::is_enum_v<U>) {
    return make_sql_value(static_cast<std::underlying_type_t<U>>(v));
  } else if constexpr (std::is_convertible_v<U, std::string>) {
    return sql_value(std::string(std::forward<T>(v)));
  } else {
    static_assert(std::is_same_v<U, U> && false,
      "type cannot be used as a query parameter; convert it to a "
      "supported sql_value type or register a converter");
  }
}

}  // namespace detail

// Ordered parameter container for prepared statements.
class UNIORM_API params {
public:
  params() = default;

  template <class... Ts>
  explicit params(Ts&&... values) {
    values_.reserve(sizeof...(Ts));
    (values_.push_back(detail::make_sql_value(std::forward<Ts>(values))), ...);
  }

  explicit params(std::vector<sql_value> values) : values_(std::move(values)) {}

  std::size_t size() const noexcept {
    return values_.size();
  }
  sql_value const& at(std::size_t index) const {
    return values_.at(index);
  }
  std::vector<sql_value> const& values() const noexcept {
    return values_;
  }

  // Internal: bind all parameters (1-based) to an ODBC statement. The
  // returned staging object must outlive statement execution.
  detail::param_staging bind(odbc::statement& stmt) const;

private:
  std::vector<sql_value> values_;
};

}  // namespace uniorm
