#include "compat.hpp"

#include <cstddef>
#include <limits>
#include <string>

#include "identifier.hpp"

namespace uniorm::odbc {

namespace {

constexpr version_triple version_max{ UINT32_MAX, 0, 0 };

std::uint32_t read_group(std::string_view text, std::size_t& position) {
  std::uint64_t value = 0;
  constexpr std::uint64_t ceiling = std::numeric_limits<std::uint32_t>::max();
  while (
    position < text.size() && text[position] >= '0' && text[position] <= '9') {
    value = value * 10 + static_cast<std::uint64_t>(text[position] - '0');
    if (value > ceiling) {
      value = ceiling;
    }
    ++position;
  }
  return static_cast<std::uint32_t>(value);
}

// The conservative prior for combos nothing has measured: MySQL-wire
// connectors granted array INSERT only, psqlODBC every op but a one-set tally,
// Oracle's ODBC driver no array DML. A version measured executable is pinned
// ahead of its family's prior; the CI probe re-measures each leg and
// integration at scale confirms before a grant widens to versions still
// unmeasured.
constexpr compat_entry compat_table[] = {
  // maodbc 3.1.12 measured locally (probe N=2..1000, integration green via the
  // array path); 3.1.23 measured on CI against MariaDB 11. Unmeasured versions
  // stay on the prior below: array INSERT granted, array UPDATE/DELETE denied
  // pending measurement.
  { "maodbc|maria", { 3, 1, 12 }, { 3, 1, 13 }, "", {}, version_max, true, true,
    true, true },
  { "maodbc|maria", { 3, 1, 23 }, { 3, 1, 24 }, "", {}, version_max, true, true,
    true, true },
  { "maodbc|maria", {}, version_max, "", {}, version_max, true, false, false,
    true },
  // myodbc 26.10.0 measured on CI against MySQL 8.4: every op passes, whole
  // tally. Older unmeasured versions stay on the insert-only prior.
  { "myodbc|mysql", { 26, 10, 0 }, { 26, 10, 1 }, "", {}, version_max, true,
    true, true, true },
  { "myodbc|mysql", {}, version_max, "", {}, version_max, true, false, false,
    true },
  { "psqlodbc|postgres", {}, version_max, "", {}, version_max, true, true, true,
    false },
  // Keyed on the DBMS, not the driver name: the ODBC driver's batch self-report
  // is unreliable, the server name is solid.
  { "", {}, version_max, "oracle", {}, version_max, false, false, false, true },
};

bool family_matches(std::string_view families, std::string_view folded_name) {
  if (families.empty()) {
    return true;
  }
  // '|' separates the spellings one family goes by: the shared library name
  // on Unix, the self-reported product name on Windows.
  std::size_t start = 0;
  while (true) {
    std::size_t const bar = families.find('|', start);
    std::string_view const family = families.substr(start,
      bar == std::string_view::npos ? std::string_view::npos : bar - start);
    if (folded_name.find(family) != std::string_view::npos) {
      return true;
    }
    if (bar == std::string_view::npos) {
      return false;
    }
    start = bar + 1;
  }
}

bool in_range(version_triple version, version_triple low, version_triple high) {
  // The ceiling sentinel means "no upper end", so the version it names is
  // inside the range too; a plain half-open test would drop the sentinel
  // value itself, which saturation and driver versions may both reach.
  return version >= low && (version < high || high == version_max);
}

}  // namespace

version_triple parse_version(std::string_view text) {
  version_triple version;
  std::size_t position = text.find_first_of("0123456789");
  if (position == std::string_view::npos) {
    return version;
  }
  version.major = read_group(text, position);
  if (position < text.size() && text[position] == '.') {
    ++position;
    version.minor = read_group(text, position);
  }
  if (position < text.size() && text[position] == '.') {
    ++position;
    version.patch = read_group(text, position);
  }
  return version;
}

compat_entry const* compat_lookup(std::string_view driver_name,
  version_triple driver_ver, std::string_view dbms_name,
  version_triple dbms_ver) {
  std::string const driver = uniorm::detail::fold_lower(driver_name);
  std::string const dbms = uniorm::detail::fold_lower(dbms_name);
  for (auto const& entry : compat_table) {
    if (family_matches(entry.driver_family, driver) &&
        family_matches(entry.dbms_family, dbms) &&
        in_range(driver_ver, entry.driver_min, entry.driver_max) &&
        in_range(dbms_ver, entry.dbms_min, entry.dbms_max)) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace uniorm::odbc