#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <uniorm/detail/traits.hpp>
#include <uniorm/error.hpp>
#include <uniorm/export.hpp>
#include <uniorm/value.hpp>

namespace uniorm {

// Convert a dynamic sql_value to T. Tolerates narrowing between integral
// widths (range-checked) and integral-to-double; everything else must match
// exactly. Throws type_mismatch.
template <class T>
T value_cast(sql_value const& v);

namespace detail {

template <class To>
To narrow_checked(sql_value const& v) {
  auto check_and_cast = [&]<class From>(From from) {
    if constexpr (std::is_integral_v<From> && std::is_integral_v<To>) {
      bool fits;
      if constexpr (std::is_signed_v<From> == std::is_signed_v<To>) {
        fits = from >= std::numeric_limits<To>::min() &&
               from <= std::numeric_limits<To>::max();
      } else if constexpr (std::is_signed_v<From>) {
        fits = from >= 0 && static_cast<std::make_unsigned_t<From>>(from) <=
                              static_cast<std::make_unsigned_t<To>>(
                                std::numeric_limits<To>::max());
      } else {
        fits = from <= static_cast<std::make_unsigned_t<From>>(
                         std::numeric_limits<To>::max());
      }
      if (!fits) {
        throw type_mismatch("numeric value out of range for target type");
      }
      return static_cast<To>(from);
    } else if constexpr (std::is_arithmetic_v<From> &&
                         std::is_arithmetic_v<To>) {
      return static_cast<To>(from);
    } else {
      throw type_mismatch("incompatible sql_value type");
    }
  };

  if (auto* p = std::get_if<std::int16_t>(&v))
    return check_and_cast(*p);
  if (auto* p = std::get_if<std::int32_t>(&v))
    return check_and_cast(*p);
  if (auto* p = std::get_if<std::int64_t>(&v))
    return check_and_cast(*p);
  if (auto* p = std::get_if<bool>(&v)) {
    if constexpr (std::is_same_v<To, bool>)
      return *p;
    else
      return check_and_cast(static_cast<int>(*p));
  }
  if (auto* p = std::get_if<double>(&v)) {
    if constexpr (std::is_floating_point_v<To>)
      return static_cast<To>(*p);
    else
      throw type_mismatch("cannot convert double to integral target");
  }
  throw type_mismatch("incompatible sql_value type");
}

}  // namespace detail

template <class T>
T value_cast(sql_value const& v) {
  if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int8_t> ||
                std::is_same_v<T, std::int16_t> ||
                std::is_same_v<T, std::int32_t> ||
                std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
    if (auto* p = std::get_if<T>(&v))
      return *p;
    return detail::narrow_checked<T>(v);
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (auto* p = std::get_if<std::string>(&v))
      return *p;
    throw type_mismatch("sql_value does not hold a string");
  } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
    if (auto* p = std::get_if<std::vector<std::byte>>(&v))
      return *p;
    throw type_mismatch("sql_value does not hold binary data");
  } else if constexpr (std::is_same_v<T, timestamp>) {
    if (auto* p = std::get_if<timestamp>(&v))
      return *p;
    throw type_mismatch("sql_value does not hold a timestamp");
  } else if constexpr (std::is_same_v<T, std::monostate>) {
    if (std::holds_alternative<std::monostate>(v))
      return {};
    throw type_mismatch("sql_value is not NULL");
  } else if constexpr (detail::is_optional_v<T>) {
    if (std::holds_alternative<std::monostate>(v))
      return std::nullopt;
    return T{ value_cast<typename T::value_type>(v) };
  } else {
    static_assert(
      std::is_same_v<T, T> && false, "unsupported value_cast target type");
  }
}

// Shared by every row of one result set: the column names plus a
// name->index map built once at describe time.
struct UNIORM_API column_names {
  std::vector<std::string> names;
  std::unordered_map<std::string, std::size_t> index;

  column_names() = default;

  explicit column_names(std::vector<std::string> column_list)
    : names(std::move(column_list)) {
    for (std::size_t i = 0; i < names.size(); ++i) {
      index.emplace(names[i], i);
    }
  }
};

// A materialized row: owned values plus a shared column-name table.
class UNIORM_API row {
public:
  row() = default;
  row(std::shared_ptr<column_names> names, std::vector<sql_value> values);

  sql_value const& at(std::string_view name) const;
  sql_value const& at(std::size_t index) const;

  template <class T>
  T get(std::string_view name) const {
    return value_cast<T>(at(name));
  }
  template <class T>
  T get(std::size_t index) const {
    return value_cast<T>(at(index));
  }

  bool is_null(std::string_view name) const {
    return is_null_value(at(name));
  }
  bool is_null(std::size_t index) const {
    return is_null_value(at(index));
  }

  std::size_t size() const noexcept {
    return values_.size();
  }
  std::vector<std::string> const& names() const noexcept {
    return names_->names;
  }

private:
  static bool is_null_value(sql_value const& v) noexcept {
    return uniorm::is_null(v);
  }

  std::shared_ptr<column_names> names_ = std::make_shared<column_names>();
  std::vector<sql_value> values_;
};

}  // namespace uniorm
