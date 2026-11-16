#include <cstdint>

#include "libpq/native_types.hpp"
#include "libpq/schema_catalog.hpp"

#include "check.hpp"

using namespace uniorm;

void test_libpq_catalog_column_metrics() {
  // An exact number carries precision above scale in the type modifier.
  int const ten_two = static_cast<int>((10 << 16) | 2) + 4;
  auto numeric =
    libpq::catalog_column_metrics(libpq::pg_type::numeric, ten_two);
  CHECK(numeric.size == 10);
  CHECK(numeric.decimals == 2);

  // A blessed variable-length type carries a width behind the varlena header.
  auto varchar = libpq::catalog_column_metrics(libpq::pg_type::varchar, 64 + 4);
  CHECK(varchar.size == 64);
  CHECK(varchar.decimals == 0);
  auto bpchar = libpq::catalog_column_metrics(libpq::pg_type::bpchar, 8 + 4);
  CHECK(bpchar.size == 8);

  // An undeclared modifier (-1) declares nothing.
  auto open_varchar =
    libpq::catalog_column_metrics(libpq::pg_type::varchar, -1);
  CHECK(open_varchar.size == 0);
  auto open_numeric =
    libpq::catalog_column_metrics(libpq::pg_type::numeric, -1);
  CHECK(open_numeric.size == 0);
  CHECK(open_numeric.decimals == 0);

  // A fixed-width type declares no size or scale of its own.
  auto integer = libpq::catalog_column_metrics(libpq::pg_type::integer, -1);
  CHECK(integer.size == 0);
  CHECK(integer.decimals == 0);
}