#include <cstdint>

#include <uniorm/types.hpp>

#include "oracle_oci/native_types.hpp"

#include "check.hpp"

using namespace uniorm;

void test_oracle_oci_number_shape() {
  // NUMBER is one code for every exact numeric, so the neutral type comes
  // from the described digits: a scale means fractional digits are stored,
  // and precision 0 is the server's own "no width declared".
  CHECK(oracle_oci::number_shape(0, 0) == sql_type::decimal);
  CHECK(oracle_oci::number_shape(1, 0) == sql_type::smallint);
  CHECK(oracle_oci::number_shape(4, 0) == sql_type::smallint);
  CHECK(oracle_oci::number_shape(5, 0) == sql_type::integer);
  CHECK(oracle_oci::number_shape(9, 0) == sql_type::integer);
  CHECK(oracle_oci::number_shape(10, 0) == sql_type::bigint);
  CHECK(oracle_oci::number_shape(18, 0) == sql_type::bigint);
  CHECK(oracle_oci::number_shape(19, 0) == sql_type::decimal);
  CHECK(oracle_oci::number_shape(10, 2) == sql_type::decimal);
  CHECK(oracle_oci::number_shape(38, -4) == sql_type::decimal);
}

void test_oracle_oci_sql_type_from_native() {
  // A native NUMBER(1,0) is what an entity's bool field binds to, which is
  // the one case the shape rule is overridden for.
  CHECK(oracle_oci::sql_type_from_native(SQLT_INT, 1, 0) == sql_type::boolean);
  CHECK(oracle_oci::sql_type_from_native(SQLT_INT, 10, 0) == sql_type::bigint);
  CHECK(oracle_oci::sql_type_from_native(SQLT_NUM, 0, 0) == sql_type::decimal);
  CHECK(oracle_oci::sql_type_from_native(SQLT_NUM, 9, 0) == sql_type::integer);
  CHECK(oracle_oci::sql_type_from_native(SQLT_VNU, 5, 2) == sql_type::decimal);
  CHECK(oracle_oci::sql_type_from_native(SQLT_PDN, 3, 0) == sql_type::smallint);
  CHECK(oracle_oci::sql_type_from_native(SQLT_UIN, 9, 0) == sql_type::integer);
  CHECK(oracle_oci::sql_type_from_native(SQLT_SLS, 9, 0) == sql_type::integer);

  CHECK(oracle_oci::sql_type_from_native(SQLT_FLT, 0, 0) == sql_type::decimal);
  CHECK(oracle_oci::sql_type_from_native(SQLT_BFLOAT, 0, 0) == sql_type::real);
  CHECK(oracle_oci::sql_type_from_native(SQLT_IBFLOAT, 0, 0) == sql_type::real);
  CHECK(oracle_oci::sql_type_from_native(SQLT_BDOUBLE, 0, 0) ==
    sql_type::double_precision);
  CHECK(oracle_oci::sql_type_from_native(SQLT_IBDOUBLE, 0, 0) ==
    sql_type::double_precision);

  CHECK(oracle_oci::sql_type_from_native(SQLT_AFC, 1, 0) ==
    sql_type::character);
  CHECK(oracle_oci::sql_type_from_native(SQLT_CHR, 10, 0) == sql_type::varchar);
  CHECK(oracle_oci::sql_type_from_native(SQLT_VCS, 10, 0) == sql_type::varchar);
  CHECK(oracle_oci::sql_type_from_native(SQLT_STR, 10, 0) == sql_type::varchar);
  CHECK(oracle_oci::sql_type_from_native(SQLT_AVC, 10, 0) ==
    sql_type::wvarchar);

  CHECK(oracle_oci::sql_type_from_native(SQLT_LNG, 0, 0) ==
    sql_type::longvarchar);
  CHECK(oracle_oci::sql_type_from_native(SQLT_LVC, 0, 0) ==
    sql_type::longvarchar);
  CHECK(oracle_oci::sql_type_from_native(SQLT_CLOB, 0, 0) ==
    sql_type::longvarchar);
  CHECK(oracle_oci::sql_type_from_native(SQLT_NCLOB, 0, 0) ==
    sql_type::wvarchar);

  CHECK(oracle_oci::sql_type_from_native(SQLT_BIN, 10, 0) ==
    sql_type::varbinary);
  CHECK(oracle_oci::sql_type_from_native(SQLT_VBI, 10, 0) ==
    sql_type::varbinary);
  CHECK(oracle_oci::sql_type_from_native(SQLT_LBI, 0, 0) == sql_type::binary);
  CHECK(oracle_oci::sql_type_from_native(SQLT_LVB, 0, 0) == sql_type::binary);
  CHECK(oracle_oci::sql_type_from_native(SQLT_BLOB, 0, 0) == sql_type::binary);

  CHECK(oracle_oci::sql_type_from_native(SQLT_DAT, 0, 0) == sql_type::date);
  CHECK(oracle_oci::sql_type_from_native(SQLT_ODT, 0, 0) == sql_type::date);
  CHECK(oracle_oci::sql_type_from_native(SQLT_DATE, 0, 0) == sql_type::date);
  CHECK(oracle_oci::sql_type_from_native(SQLT_TIMESTAMP, 0, 0) ==
    sql_type::timestamp);
  CHECK(oracle_oci::sql_type_from_native(SQLT_TIMESTAMP_TZ, 0, 0) ==
    sql_type::timestamp);
  CHECK(oracle_oci::sql_type_from_native(SQLT_TIMESTAMP_LTZ, 0, 0) ==
    sql_type::timestamp);
  CHECK(oracle_oci::sql_type_from_native(SQLT_TIME, 0, 0) == sql_type::time);
  CHECK(oracle_oci::sql_type_from_native(SQLT_TIME_TZ, 0, 0) == sql_type::time);

  CHECK(oracle_oci::sql_type_from_native(SQLT_RDD, 0, 0) == sql_type::guid);
  CHECK(oracle_oci::sql_type_from_native(SQLT_RID, 0, 0) == sql_type::guid);
  // A type the neutral set has no shape for is named, not guessed at.
  CHECK(oracle_oci::sql_type_from_native(SQLT_CUR, 0, 0) == sql_type::other);
  CHECK(oracle_oci::sql_type_from_native(SQLT_VST, 0, 0) == sql_type::other);
}

void test_oracle_oci_wide_columns() {
  // Whether the value comes when a row is fetched: a LOB arrives as a
  // locator that has to be read, a LONG arrives truncated to the buffer it
  // was defined with, and neither length is trustworthy.
  CHECK(oracle_oci::is_locator(SQLT_CLOB));
  CHECK(oracle_oci::is_locator(SQLT_BLOB));
  CHECK(oracle_oci::is_locator(SQLT_BFILEE));
  CHECK(!oracle_oci::is_locator(SQLT_LNG));
  CHECK(!oracle_oci::is_locator(SQLT_NCLOB));
  CHECK(!oracle_oci::is_locator(SQLT_CHR));

  CHECK(oracle_oci::needs_wide_staging(SQLT_LNG));
  CHECK(oracle_oci::needs_wide_staging(SQLT_LVC));
  CHECK(oracle_oci::needs_wide_staging(SQLT_LBI));
  CHECK(oracle_oci::needs_wide_staging(SQLT_LVB));
  CHECK(oracle_oci::needs_wide_staging(SQLT_NCLOB));
  CHECK(!oracle_oci::needs_wide_staging(SQLT_CLOB));
  CHECK(!oracle_oci::needs_wide_staging(SQLT_VCS));

  // Text or bytes decides what a continuation read of a wide value returns;
  // a BFILE points at an OS file of bytes.
  CHECK(oracle_oci::is_wide_text(SQLT_CLOB));
  CHECK(oracle_oci::is_wide_text(SQLT_LNG));
  CHECK(oracle_oci::is_wide_text(SQLT_LVC));
  CHECK(!oracle_oci::is_wide_text(SQLT_BLOB));
  CHECK(!oracle_oci::is_wide_text(SQLT_BFILEE));
  CHECK(!oracle_oci::is_wide_text(SQLT_LVB));
  CHECK(!oracle_oci::is_wide_text(SQLT_CHR));
}
