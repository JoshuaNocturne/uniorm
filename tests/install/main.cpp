// Consumes uniorm the way an outside project does: installed headers, the
// flags the imported target carries, and the shared library as it is loaded at
// run time. Every call below lands in libuniorm.so rather than in a header, so
// reaching it proves the link line and the load path both work.

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

  // Scheme resolution happens inside the .so; an unregistered scheme must fail
  // there instead of quietly falling back to the default backend.
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
  expect(uniorm::backend::registry::instance().contains("odbc"),
    "the ODBC backend self-registered on load");
#else
  // Core-only shape: no backend is linked, so the "odbc" scheme a bare
  // connection string parses to must fail at registry::create, not by
  // silently falling back to anything.
  expect(!uniorm::backend::registry::instance().contains("odbc"),
    "with no backend linked, the odbc scheme is unregistered");
  bool bare_rejected = false;
  try {
    uniorm::connection c("DSN=whatever;UID=u;PWD=p");
  } catch (uniorm::backend::unknown_scheme const&) {
    bare_rejected = true;
  } catch (std::exception const& e) {
    std::cerr << "bare DSN threw the wrong type: " << e.what() << '\n';
    ++failures;
  }
  expect(bare_rejected, "a bare DSN without a linked backend is unknown_scheme");
#endif

  if (failures != 0) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "installed uniorm is consumable\n";
  return 0;
}
