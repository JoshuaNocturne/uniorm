#include <cstdint>
#include <string_view>

#include "odbc/compat.hpp"

#include "check.hpp"

using namespace uniorm;

namespace {

odbc::version_triple version(
  std::uint32_t major, std::uint32_t minor, std::uint32_t patch) {
  return odbc::version_triple{ major, minor, patch };
}

}  // namespace

void test_odbc_compat() {
  // The first three decimal groups, tolerant of leading zeros and trailing
  // text; a missing group reads as zero and no digits as "did not say".
  CHECK(odbc::parse_version("03.01.0023") == version(3, 1, 23));
  CHECK(odbc::parse_version("23.26.3.0.0") == version(23, 26, 3));
  CHECK(odbc::parse_version("11.08.0009") == version(11, 8, 9));
  CHECK(odbc::parse_version("3.1") == version(3, 1, 0));
  CHECK(odbc::parse_version("3") == version(3, 0, 0));
  CHECK(odbc::parse_version("") == version(0, 0, 0));
  CHECK(odbc::parse_version("no digits here") == version(0, 0, 0));
  CHECK(odbc::parse_version("PostgreSQL 18.6") == version(18, 6, 0));

  // Patch level is distinguishable: the dialect's major*100+minor collapses
  // both of these to 301, the table must not.
  CHECK(version(3, 1, 12) < version(3, 1, 23));
  CHECK(version(3, 1, 23) > version(3, 1, 12));
  CHECK(version(3, 1, 12) == version(3, 1, 12));
  // Half-open upper bound: the boundary version itself is excluded.
  CHECK(version(3, 1, 0) >= version(3, 1, 0));
  CHECK(version(3, 1, 999) < version(3, 2, 0));
  CHECK(!(version(3, 2, 0) < version(3, 2, 0)));

  // maodbc 3.1.12 is measured executable: every op granted, whole tally.
  {
    auto const* entry = odbc::compat_lookup(
      "maodbc.so", version(3, 1, 12), "MariaDB", version(10, 6, 0));
    CHECK(entry != nullptr);
    CHECK(entry->array_insert);
    CHECK(entry->array_update);
    CHECK(entry->array_delete);
    CHECK(entry->array_rowcount_totals);
  }
  // An unmeasured maodbc version stays on the conservative family prior: the
  // grant is pinned to the version the probe and integration confirmed.
  {
    auto const* entry = odbc::compat_lookup(
      "maodbc.so", version(3, 1, 23), "MariaDB", version(11, 0, 0));
    CHECK(entry != nullptr);
    CHECK(entry->array_insert);
    CHECK(!entry->array_update);
    CHECK(!entry->array_delete);
    CHECK(entry->array_rowcount_totals);
  }
  // myodbc unmeasured: array INSERT only, total row counts.
  {
    auto const* entry = odbc::compat_lookup(
      "myodbc8w.so", version(8, 0, 32), "MySQL", version(8, 0, 0));
    CHECK(entry != nullptr);
    CHECK(entry->array_insert);
    CHECK(!entry->array_update);
    CHECK(!entry->array_delete);
    CHECK(entry->array_rowcount_totals);
  }
  // psqlODBC: every op runs, but the tally is one set's count.
  {
    auto const* entry = odbc::compat_lookup(
      "psqlodbcw.so", version(13, 2, 0), "PostgreSQL", version(18, 6, 0));
    CHECK(entry != nullptr);
    CHECK(entry->array_insert);
    CHECK(entry->array_update);
    CHECK(entry->array_delete);
    CHECK(!entry->array_rowcount_totals);
  }
  // Oracle keyed on the DBMS, whatever the driver calls itself.
  {
    auto const* entry = odbc::compat_lookup(
      "SQORA32.DLL", version(11, 8, 9), "Oracle", version(19, 0, 0));
    CHECK(entry != nullptr);
    CHECK(!entry->array_insert);
    CHECK(!entry->array_update);
    CHECK(!entry->array_delete);
    CHECK(entry->array_rowcount_totals);
  }

  // Family matching folds ASCII case, so the driver's own casing is irrelevant.
  CHECK(odbc::compat_lookup("MAODBC.SO", version(3, 1, 12), "MariaDB",
          version(10, 6, 0)) != nullptr);

  // The unbounded sentinel admits both a zero version and a huge one.
  CHECK(odbc::compat_lookup("maodbc.so", version(0, 0, 0), "MariaDB",
          version(0, 0, 0)) != nullptr);
  CHECK(odbc::compat_lookup("maodbc.so", version(UINT32_MAX - 1, 0, 0),
          "MariaDB", version(0, 0, 0)) != nullptr);

  // An unknown combo is a miss: the caller falls back to conservative deny.
  CHECK(odbc::compat_lookup("freetds.so", version(1, 0, 0),
          "Microsoft SQL Server", version(15, 0, 0)) == nullptr);
  CHECK(
    odbc::compat_lookup("", version(0, 0, 0), "", version(0, 0, 0)) == nullptr);
}