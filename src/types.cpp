#include "uniorm/types.hpp"

#include <sql.h>
#include <sqlext.h>

namespace uniorm {

sql_type sql_type_from_native(int t) {
  switch (t) {
  case SQL_BIT:
    return sql_type::boolean;
  case SQL_TINYINT:
  case SQL_SMALLINT:
    return sql_type::smallint;
  case SQL_INTEGER:
    return sql_type::integer;
  case SQL_BIGINT:
    return sql_type::bigint;
  case SQL_REAL:
  case SQL_FLOAT:
    return sql_type::real;
  case SQL_DOUBLE:
    return sql_type::double_precision;
  case SQL_DECIMAL:
  case SQL_NUMERIC:
    return sql_type::decimal;
  case SQL_CHAR:
    return sql_type::character;
  case SQL_VARCHAR:
    return sql_type::varchar;
  case SQL_LONGVARCHAR:
    return sql_type::longvarchar;
  case SQL_WCHAR:
    return sql_type::wchar;
  case SQL_WVARCHAR:
    return sql_type::wvarchar;
  case SQL_WLONGVARCHAR:
    return sql_type::wvarchar;
  case SQL_BINARY:
    return sql_type::binary;
  case SQL_VARBINARY:
    return sql_type::varbinary;
  case SQL_LONGVARBINARY:
    return sql_type::varbinary;
  case SQL_TYPE_DATE:
  case SQL_DATE:
    return sql_type::date;
  case SQL_TYPE_TIME:
  case SQL_TIME:
    return sql_type::time;
  case SQL_TYPE_TIMESTAMP:
  case SQL_TIMESTAMP:
    return sql_type::timestamp;
  case SQL_GUID:
    return sql_type::guid;
  default:
    return sql_type::other;
  }
}

}  // namespace uniorm
