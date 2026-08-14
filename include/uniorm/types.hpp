#pragma once

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

}  // namespace uniorm
