// Consumes uniorm the way an outside project does: installed headers, the
// flags the imported target carries, and the shared library as it is loaded at
// run time. Every call below lands in libuniorm.so rather than in a header, so
// reaching it proves the link line and the load path both work. Backends are
// not linked here at all -- the core dlopens them on scheme miss, so this
// binary is exactly the same for both smoke shapes and only the plugin
// directory differs.

#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <uniorm/uniorm.hpp>

#include <uniorm/async.hpp>
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

// A caller-owned executor. submit() runs the work on the calling thread, so
// the task below is decided before the drive call returns.
class inline_executor final : public uniorm::async::executor {
public:
  void submit(std::function<void()> work) override {
    ++submissions;
    work();
  }

  int submissions = 0;
};

uniorm::async::task<int> doubled(int from) {
  co_return from * 2;
}

uniorm::async::task<int> chained() {
  int const first = co_await doubled(1);
  int const second = co_await doubled(first);
  co_return second;
}

int explode() {
  throw std::runtime_error("installed async body");
}

// The throw sits inside the co_return's own expression: a body naming no
// coroutine keyword is an ordinary function whose throw never reaches the
// promise.
uniorm::async::task<int> throwing() {
  co_return explode();
}

}  // namespace

int main() {
  uniorm::dialect const mysql = uniorm::dialect::detect("MariaDB");
  expect(mysql.quote_identifier("t") == "`t`", "MariaDB quotes with backticks");
  expect(!mysql.ansi_pagination, "MariaDB pages with LIMIT/OFFSET");
  uniorm::dialect const generic = uniorm::dialect::detect("PostgreSQL");
  expect(generic.quote_identifier("t") == "\"t\"", "ANSI quoting by default");
  uniorm::dialect const oracle = uniorm::dialect::detect("Oracle");
  expect(oracle.quote_identifier("t") == "\"T\"",
    "a dialect folds what its server folds");
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

  // Async: the exported pool symbols and the await chain have to resolve out
  // of libuniorm, not just compile. Size it before first use, which is the
  // shape the documentation tells a starter to write.
  uniorm::async::default_executor_size(2);
  expect(chained().block() == 4, "an awaited chain returns through the pool");

  inline_executor here;
  expect(doubled(21).block(here) == 42, "a caller's executor runs the body");
  expect(here.submissions == 1, "one drive action submits once");

  bool rethrown = false;
  try {
    throwing().block(here);
  } catch (std::runtime_error const&) {
    rethrown = true;
  }
  expect(rethrown, "a body's exception reaches the drive action");

  bool refused = false;
  auto driven_once = doubled(3);
  driven_once.block(here);
  try {
    driven_once.block(here);
  } catch (uniorm::uniorm_error const&) {
    refused = true;
  }
  expect(refused, "a second drive action on one task is refused");

  std::exception_ptr reported;
  throwing().start(here,
    [&](std::exception_ptr error) { reported = error; });
  expect(static_cast<bool>(reported),
    "a detached failure reports through its handler");

  // The session face is the part whose bodies sit in the library: making a
  // task runs nothing, so this reaches execute_update's own definition, the
  // core's refusal to work with no connection, and ~async::orm's wait -- with
  // no database in reach.
  bool unconnected = false;
  {
    uniorm::async::orm session;
    expect(session.pending() == 0, "a fresh session has nothing queued");
    expect(session.submit_limit() > 0, "a session has a submit limit");
    try {
      session.execute_update("UPDATE contacts SET name = 'x'").block(here);
    } catch (uniorm::uniorm_error const&) {
      unconnected = true;
    }
    expect(unconnected, "an operation with no connection throws");
    expect(session.pending() == 0, "a thrown operation frees the session");

    // begin() is the one terminal that hands back a handle instead of a count.
    // With nothing connected its body throws before any handle exists, so this
    // covers the task's own definition in the library -- ~async::transaction is
    // the one the integration leg reaches, with a connection behind it.
    bool no_transaction = false;
    try {
      session.begin().block(here);
    } catch (uniorm::uniorm_error const&) {
      no_transaction = true;
    }
    expect(no_transaction, "begin with no connection throws");
    expect(session.pending() == 0, "a refused begin leaves the session quiet");
  }

  if (failures != 0) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "installed uniorm is consumable\n";
  return 0;
}
