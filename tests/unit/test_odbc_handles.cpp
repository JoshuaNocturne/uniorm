#include <string>
#include <utility>

#include <uniorm/backend/error.hpp>

#include "odbc/connection.hpp"
#include "odbc/environment.hpp"
#include "odbc/error.hpp"

#include "check.hpp"

using namespace uniorm;

void test_odbc_handles() {
  // Handle allocation talks only to the driver manager; no DSN required.
  // Statements are covered by integration tests: ODBC refuses to allocate
  // one on an unopened connection.
  odbc::environment env;
  CHECK(env.native() != nullptr);

  odbc::environment moved = std::move(env);
  CHECK(moved.native() != nullptr);

  odbc::connection conn(moved);
  CHECK(conn.native() != nullptr);
  CHECK(!conn.is_open());

  odbc::connection moved_conn = std::move(conn);
  CHECK(!moved_conn.is_open());
}

void test_odbc_error_is_backend_error() {
  // The core throws nothing of its own on a driver failure: odbc_error is a
  // backend_error, so callers classify SQL errors through the public contract
  // and keep the diagnostic records.
  try {
    throw odbc::odbc_error("execute", { { "42S02", 1146, "no such table" } });
  } catch (backend::backend_error const& e) {
    CHECK(e.backend_name() == "odbc");
    CHECK(e.diagnostics().size() == 1);
    if (e.diagnostics().size() == 1) {
      CHECK(e.diagnostics()[0].state == "42S02");
      CHECK(e.diagnostics()[0].native_code == 1146);
    }
    CHECK(std::string(e.what()).find("[42S02] (1146) no such table") !=
      std::string::npos);
  }
}
