// Consumes uniorm the way an outside project does: installed headers, the
// flags the imported target carries, and the shared library as it is loaded at
// run time. Every call below lands in libuniorm.so rather than in a header, so
// reaching it proves the link line and the load path both work. Backends are
// not linked here at all -- the core dlopens them on scheme miss, so this
// binary is exactly the same for both smoke shapes and only the plugin
// directory differs.

#include <exception>
#include <iostream>
#include <string>

#include <uniorm/uniorm.hpp>

#include <uniorm/backend/error.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/connection.hpp>

namespace {

int failures = 0;

void expect(bool ok, char const* what) {
  if (!ok) {
    std::cerr << "failed: " << what << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  uniorm::dialect const mysql = uniorm::dialect::detect("MariaDB");
  expect(mysql.quote_identifier("t") == "`t`", "MariaDB quotes with backticks");
  expect(!mysql.ansi_pagination, "MariaDB pages with LIMIT/OFFSET");
  uniorm::dialect const generic = uniorm::dialect::detect("PostgreSQL");
  expect(generic.quote_identifier("t") == "\"t\"", "ANSI quoting by default");
  uniorm::sql_type const dec = uniorm::sql_type::decimal;
  expect(std::string(uniorm::sql_type_name(dec)) == "DECIMAL",
    "sql_type_name names DECIMAL");

  expect(uniorm::backend::parse_scheme("DSN=x;UID=y").scheme == "odbc",
    "a bare connection string is ODBC");

  // Loading is by scheme, not by link: a scheme with no plugin file next to
  // it must fail at registry::create, not by silently falling back to the
  // default backend.
  bool rejected = false;
  try {
    uniorm::connection c("nosuchbackend://x");
  } catch (uniorm::backend::unknown_scheme const&) {
    rejected = true;
  } catch (std::exception const& e) {
    std::cerr << "wrong exception type: " << e.what() << '\n';
    ++failures;
  }
  expect(rejected, "unknown scheme rejected");

#ifdef UNIORM_SMOKE_HAS_ODBC
  // The plugin is on disk. A bare DSN should get past registry::create --
  // the loader dlopens it, calls register, and the factory then fails on
  // the missing DSN itself. What matters is that the error is no longer
  // unknown_scheme and that odbc shows up in the map afterwards.
  expect(!uniorm::backend::registry::instance().contains("odbc"),
    "before any load, the odbc scheme is not registered");
  bool reached_odbc = false;
  bool fell_back_to_unknown = false;
  try {
    uniorm::connection c("DSN=nonexistent_dsm_for_smoke;UID=u;PWD=p");
    reached_odbc = true;
  } catch (uniorm::backend::unknown_scheme const&) {
    fell_back_to_unknown = true;
  } catch (std::exception const&) {
    reached_odbc = true;
  }
  expect(!fell_back_to_unknown,
    "with the plugin present, a bare DSN is not unknown_scheme");
  expect(reached_odbc, "the load-on-miss reached the ODBC layer");
  expect(uniorm::backend::registry::instance().contains("odbc"),
    "after the load-on-miss, odbc is registered");
#else
  // Core-only install: the plugin file is not there, so a bare DSN has to
  // remain unknown_scheme and odbc has to stay unregistered -- a missing
  // plugin must never become an implicit fallback to any other backend.
  bool bare_rejected = false;
  try {
    uniorm::connection c("DSN=whatever;UID=u;PWD=p");
  } catch (uniorm::backend::unknown_scheme const&) {
    bare_rejected = true;
  } catch (std::exception const& e) {
    std::cerr << "bare DSN threw the wrong type: " << e.what() << '\n';
    ++failures;
  }
  expect(bare_rejected, "a bare DSN with no plugin installed is unknown_scheme");
  expect(!uniorm::backend::registry::instance().contains("odbc"),
    "with no plugin installed, odbc never registers");
#endif

  if (failures != 0) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "installed uniorm is consumable\n";
  return 0;
}
