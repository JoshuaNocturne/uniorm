// The CI compatibility-table validator: it opens the leg's real driver, reads
// the identity the runtime table keys on, and compares the bits the table chose
// (conn.caps()) against what an effect-level probe measures on a scratch table
// across a sweep of parameter-set sizes. The probe core is the same one the
// runtime fallback uses, so this validates the whole gating path end to end.
//
// The assertion is bidirectional now that every CI leg's table is seeded from
// measurement: a grant measurement contradicts is red (unsafe), and a denial
// measurement passes is red too (a stale prior -- a driver version bump lands
// here to be re-measured and re-pinned, not silently under-granted).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

#include <uniorm/backend/backend.hpp>

#include "odbc/backend.hpp"
#include "odbc/compat.hpp"

#include "array_probe.hpp"

using namespace uniorm;

namespace {

std::string info_string(SQLHDBC dbc, SQLUSMALLINT type) {
  char buffer[256] = {};
  SQLSMALLINT length = 0;
  if (!SQL_SUCCEEDED(SQLGetInfo(dbc, type, buffer, sizeof(buffer), &length)) ||
      length <= 0) {
    return {};
  }
  auto const count = static_cast<std::size_t>(std::min<SQLSMALLINT>(
    length, static_cast<SQLSMALLINT>(sizeof(buffer) - 1)));
  return std::string(buffer, count);
}

std::string version_text(odbc::version_triple version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor) +
         "." + std::to_string(version.patch);
}

char const* outcome_name(detail::probe_outcome outcome) {
  switch (outcome) {
  case detail::probe_outcome::pass:
    return "pass";
  case detail::probe_outcome::fail:
    return "fail";
  case detail::probe_outcome::inconclusive:
    return "inconclusive";
  }
  return "?";
}

int outcome_rank(detail::probe_outcome outcome) {
  switch (outcome) {
  case detail::probe_outcome::pass:
    return 0;
  case detail::probe_outcome::inconclusive:
    return 1;
  case detail::probe_outcome::fail:
    return 2;
  }
  return 1;
}

detail::probe_outcome worse(
  detail::probe_outcome left, detail::probe_outcome right) {
  return outcome_rank(left) >= outcome_rank(right) ? left : right;
}

// Seeded at pass, the identity for worse(): the aggregate is the worst verdict
// any swept N actually produced. Seeding at inconclusive would outrank every
// pass and report a clean sweep as inconclusive, asserting nothing.
struct measured {
  detail::probe_outcome insert = detail::probe_outcome::pass;
  detail::probe_outcome update = detail::probe_outcome::pass;
  detail::probe_outcome remove = detail::probe_outcome::pass;
  bool totals = true;
};

// One op's verdict against the table bit. Any disagreement is red in both
// directions; inconclusive is not asserted (a privilege or transport problem
// is not a capability verdict).
bool check_op(char const* op, bool table_bit, detail::probe_outcome result) {
  if (result == detail::probe_outcome::inconclusive) {
    std::printf("  %s: inconclusive, not asserted\n", op);
    return false;
  }
  bool const measured_pass = result == detail::probe_outcome::pass;
  if (table_bit != measured_pass) {
    std::printf("  %s: RED table %s but measurement %s\n", op,
      table_bit ? "grants" : "denies", measured_pass ? "passes" : "fails");
    return true;
  }
  std::printf("  %s: agrees (%s)\n", op, measured_pass ? "pass" : "fail");
  return false;
}

}  // namespace

int main() {
  char const* dsn_env = std::getenv("UNIORM_IT_DSN");
  char const* user_env = std::getenv("UNIORM_IT_USER");
  char const* pwd_env = std::getenv("UNIORM_IT_PWD");
  if (!dsn_env || !*dsn_env || !user_env || !*user_env || !pwd_env ||
      !*pwd_env) {
    std::printf(
      "skip: set UNIORM_IT_DSN / UNIORM_IT_USER / UNIORM_IT_PWD to run the "
      "array-DML probe\n");
    return 77;
  }

  odbc::backend_connection conn;
  try {
    conn.open(odbc::dsn_string(dsn_env, user_env, pwd_env));
  } catch (std::exception const& error) {
    std::printf("skip: cannot connect to '%s': %s\n", dsn_env, error.what());
    return 77;
  }

  auto* dbc = static_cast<SQLHDBC>(conn.native_handle());
  std::string const driver = info_string(dbc, SQL_DRIVER_NAME);
  odbc::version_triple const driver_ver =
    odbc::parse_version(info_string(dbc, SQL_DRIVER_VER));
  std::string const dbms = info_string(dbc, SQL_DBMS_NAME);
  odbc::version_triple const dbms_ver =
    odbc::parse_version(info_string(dbc, SQL_DBMS_VER));
  backend::capabilities const table = conn.caps();

  std::printf(
    "PROBE driver=%s dver=%s dbms=%s sver=%s | table insert=%d update=%d "
    "delete=%d totals=%d\n",
    driver.c_str(), version_text(driver_ver).c_str(), dbms.c_str(),
    version_text(dbms_ver).c_str(), table.array_insert ? 1 : 0,
    table.array_update ? 1 : 0, table.array_delete ? 1 : 0,
    table.array_rowcount_totals ? 1 : 0);

  // The worst result across the sweep decides: an op that fails at any size is
  // unsafe to grant, and 1000 is the core's default parameter-set size.
  constexpr std::array<std::size_t, 4> sweep{ 2, 8, 64, 1000 };
  measured ground;
  for (std::size_t const size : sweep) {
    detail::array_probe_result const result =
      detail::probe_array_dml(conn, size);
    ground.insert = worse(ground.insert, result.insert);
    ground.update = worse(ground.update, result.update);
    ground.remove = worse(ground.remove, result.remove);
    ground.totals = ground.totals && result.rowcount_totals;
    std::printf("  N=%zu insert=%s update=%s delete=%s totals=%d\n", size,
      outcome_name(result.insert), outcome_name(result.update),
      outcome_name(result.remove), result.rowcount_totals ? 1 : 0);
  }
  std::printf("PROBE measured insert=%s update=%s delete=%s totals=%d\n",
    outcome_name(ground.insert), outcome_name(ground.update),
    outcome_name(ground.remove), ground.totals ? 1 : 0);

  bool red = false;
  red |= check_op("insert", table.array_insert, ground.insert);
  red |= check_op("update", table.array_update, ground.update);
  red |= check_op("delete", table.array_delete, ground.remove);
  if (table.array_rowcount_totals != ground.totals) {
    std::printf("  totals: RED table says %d but measurement tallies %d\n",
      table.array_rowcount_totals ? 1 : 0, ground.totals ? 1 : 0);
    red = true;
  } else {
    std::printf("  totals: agrees (%d)\n", ground.totals ? 1 : 0);
  }

  conn.close();
  if (red) {
    std::printf("FAIL: compatibility table disagrees with measurement\n");
    return 1;
  }
  std::printf("pass: table matches measurement on every asserted op\n");
  return 0;
}