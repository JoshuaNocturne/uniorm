#include <utility>

#include <uniorm/odbc/connection.hpp>
#include <uniorm/odbc/environment.hpp>

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
