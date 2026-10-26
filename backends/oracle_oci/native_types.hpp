#pragma once

// Private header: Oracle's own type codes translated to the neutral ones.
// Not installed. This is the one place the SQLT_ numbers are named, so the
// core library builds with no Oracle header on its include path.

#include <oci.h>

#include <uniorm/types.hpp>

// The headers name CLOB and BLOB but stop short of the national-character
// LOB, which the server reports as its own code for an NCLOB column.
#ifndef SQLT_NCLOB
#define SQLT_NCLOB 117
#endif

namespace uniorm::oracle_oci {

// NUMBER is one Oracle code for every exact numeric the core distinguishes,
// so the neutral type comes from the described precision and scale: an
// integer that fits 9 digits is an integer, one that fits 18 is a bigint,
// and anything wider or unscaled is a decimal.
inline sql_type number_shape(std::int32_t precision, std::int32_t scale) {
  if (scale != 0 || precision == 0) {
    return sql_type::decimal;
  }
  if (precision <= 4) {
    return sql_type::smallint;
  }
  if (precision <= 9) {
    return sql_type::integer;
  }
  if (precision <= 18) {
    return sql_type::bigint;
  }
  return sql_type::decimal;
}

inline sql_type sql_type_from_native(
  ub2 dty, std::int32_t precision, std::int32_t scale) {
  switch (dty) {
  case SQLT_INT:
    // A native NUMBER(1,0) column is what an entity's bool binds to.
    return precision == 1 ? sql_type::boolean : number_shape(precision, scale);
  case SQLT_NUM:
  case SQLT_VNU:
  case SQLT_UIN:
  case SQLT_SLS:
  case SQLT_PDN:
    return number_shape(precision, scale);
  case SQLT_BFLOAT:
  case SQLT_IBFLOAT:
    return sql_type::real;
  case SQLT_BDOUBLE:
  case SQLT_IBDOUBLE:
    return sql_type::double_precision;
  case SQLT_FLT:
    return sql_type::decimal;
  case SQLT_AFC:
    return sql_type::character;
  case SQLT_CHR:
  case SQLT_VCS:
  case SQLT_STR:
    return sql_type::varchar;
  case SQLT_AVC:
    return sql_type::wvarchar;
  case SQLT_LNG:
  case SQLT_LVC:
    return sql_type::longvarchar;
  case SQLT_CLOB:
    return sql_type::longvarchar;
  case SQLT_NCLOB:
    return sql_type::wvarchar;
  case SQLT_BIN:
  case SQLT_VBI:
    return sql_type::varbinary;
  case SQLT_LBI:
  case SQLT_LVB:
    return sql_type::binary;
  case SQLT_BLOB:
    return sql_type::binary;
  case SQLT_DAT:
  case SQLT_ODT:
    return sql_type::date;
  case SQLT_DATE:
    return sql_type::date;
  case SQLT_TIMESTAMP:
  case SQLT_TIMESTAMP_TZ:
  case SQLT_TIMESTAMP_LTZ:
    return sql_type::timestamp;
  case SQLT_TIME:
  case SQLT_TIME_TZ:
    return sql_type::time;
  case SQLT_RDD:
  case SQLT_RID:
    return sql_type::guid;
  default:
    return sql_type::other;
  }
}

// The columns whose value does not come when a row is fetched: a LOB arrives
// as a locator that has to be read, and a LONG arrives truncated to whatever
// buffer it was defined with. Neither length is trustworthy at fetch time.
inline bool is_locator(ub2 dty) {
  switch (dty) {
  case SQLT_CLOB:
  case SQLT_BLOB:
  case SQLT_BFILEE:
    return true;
  default:
    return false;
  }
}

// A value the describe gives no trustworthy width for, fetched inline: a LONG
// or LONG RAW, which truncates to whatever buffer it was defined with, or an
// NCLOB fetched as text. The only cure is the widest buffer OCI takes.
inline bool needs_wide_staging(ub2 dty) {
  switch (dty) {
  case SQLT_LNG:
  case SQLT_LVC:
  case SQLT_LBI:
  case SQLT_LVB:
  case SQLT_NCLOB:
    return true;
  default:
    return false;
  }
}

// Whether a wide column carries text or bytes, which decides what a
// continuation read of it returns. A BFILE points at an OS file of bytes.
inline bool is_wide_text(ub2 dty) {
  switch (dty) {
  case SQLT_CLOB:
  case SQLT_NCLOB:
  case SQLT_LNG:
  case SQLT_LVC:
    return true;
  default:
    return false;
  }
}

}  // namespace uniorm::oracle_oci
