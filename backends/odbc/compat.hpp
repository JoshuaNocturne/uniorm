#pragma once

// Private header: the versioned compatibility table that decides ODBC array
// DML executability. Not installed. The static judgment is only a prior for
// known (driver, version, dbms) combos, seeded by effect-level measurement (the
// CI probe); an unknown combo falls back to conservative deny.

#include <cstdint>
#include <string_view>

#include "export.hpp"

namespace uniorm::odbc {

// The dialect's server_version collapses to major*100+minor and cannot tell
// 3.1.12 from 3.1.23; array-DML behavior drifts at patch level, so the table
// carries the third component and compares lexicographically.
struct version_triple {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
  auto operator<=>(version_triple const&) const = default;
};

// Reads the first three dot-separated decimal groups, tolerating leading zeros
// ("03.01.0023"), trailing text ("23.26.3.0.0") and missing groups. No digits
// reads as {0,0,0}: a driver that did not say.
UNIORM_ODBC_API version_triple parse_version(std::string_view text);

struct compat_entry {
  std::string_view
    driver_family;  // substring of folded SQL_DRIVER_NAME; "" = any
  version_triple driver_min;
  version_triple
    driver_max;  // half-open [min, max); UINT32_MAX major = unbounded
  std::string_view dbms_family;  // substring of folded SQL_DBMS_NAME; "" = any
  version_triple dbms_min;
  version_triple dbms_max;
  bool array_insert;
  bool array_update;
  bool array_delete;
  bool array_rowcount_totals;
};

// First matching entry, most-specific-first; nullptr when no combo is known.
// Ranges for one (driver_family, dbms_family) must not overlap.
UNIORM_ODBC_API compat_entry const* compat_lookup(std::string_view driver_name,
  version_triple driver_ver, std::string_view dbms_name,
  version_triple dbms_ver);

}  // namespace uniorm::odbc