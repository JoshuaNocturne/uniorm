#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <uniorm/export.hpp>

namespace uniorm {

// Backend-neutral SQL type classification. Every backend translates its own
// native type codes to these values behind the interface; the ODBC one keeps
// that table in src/odbc/native_types.hpp.
enum class sql_type {
  boolean,
  smallint,
  integer,
  bigint,
  real,
  double_precision,
  decimal,
  character,
  varchar,
  longvarchar,
  wchar,
  wvarchar,
  binary,
  varbinary,
  date,
  time,
  timestamp,
  guid,
  other
};

// A set of sql_type values as a bitmask, so a mapped column can name the SQL
// types its representation binds without carrying a container.
using sql_type_set = std::uint64_t;

constexpr sql_type_set sql_type_bit(sql_type t) noexcept {
  return std::uint64_t{ 1 } << static_cast<std::uint64_t>(t);
}

static_assert(
  static_cast<std::uint64_t>(sql_type::other) < sizeof(sql_type_set) * 8,
  "sql_type_set carries one bit per sql_type value");

// Printable name of a neutral SQL type, for diagnostics.
UNIORM_API char const* sql_type_name(sql_type t) noexcept;

// Metadata describing one result column; backend-neutral so the backend
// contract can expose it without depending on result_set.
struct column_info {
  std::string name;
  sql_type type;
  std::size_t display_size;  // DECIMAL/NUMERIC: the precision
  bool nullable;
  // Appended, not inserted: consumers on older 0.1 headers must still read the
  // fields they know at the same offset.
  std::size_t scale = 0;  // digits after the radix point
};

}  // namespace uniorm
