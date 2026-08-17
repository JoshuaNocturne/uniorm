#pragma once

#include <cstddef>
#include <string>

#include <uniorm/export.hpp>

namespace uniorm {

// Backend-neutral SQL type classification. Mapping from driver-native type
// codes is done by sql_type_from_native (implemented in src to keep driver
// headers out of this header).
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

UNIORM_API sql_type sql_type_from_native(int native_type);

// Metadata describing one result column; backend-neutral so the backend
// contract can expose it without depending on result_set.
struct column_info {
  std::string name;
  sql_type type;
  std::size_t display_size;
  bool nullable;
};

}  // namespace uniorm
