#include "uniorm/types.hpp"

namespace uniorm {

char const* sql_type_name(sql_type t) noexcept {
  switch (t) {
  case sql_type::boolean:
    return "BOOLEAN";
  case sql_type::smallint:
    return "SMALLINT";
  case sql_type::integer:
    return "INTEGER";
  case sql_type::bigint:
    return "BIGINT";
  case sql_type::real:
    return "REAL";
  case sql_type::double_precision:
    return "DOUBLE PRECISION";
  case sql_type::decimal:
    return "DECIMAL";
  case sql_type::character:
    return "CHAR";
  case sql_type::varchar:
    return "VARCHAR";
  case sql_type::longvarchar:
    return "LONGVARCHAR";
  case sql_type::wchar:
    return "WCHAR";
  case sql_type::wvarchar:
    return "WVARCHAR";
  case sql_type::binary:
    return "BINARY";
  case sql_type::varbinary:
    return "VARBINARY";
  case sql_type::date:
    return "DATE";
  case sql_type::time:
    return "TIME";
  case sql_type::timestamp:
    return "TIMESTAMP";
  case sql_type::guid:
    return "GUID";
  case sql_type::other:
    break;
  }
  return "OTHER";
}

}  // namespace uniorm
