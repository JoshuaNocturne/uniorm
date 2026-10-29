#include <string>

#include <uniorm/backend/error.hpp>

#include "libpq/connection.hpp"
#include "libpq/error.hpp"

#include "check.hpp"

using namespace uniorm;

void test_libpq_conninfo_keeps_a_written_target() {
  // A tail already in one of libpq's grammars is handed over as it stands;
  // only a bare host has the scheme put back on it.
  CHECK(libpq::conninfo_of("") == "");
  CHECK(libpq::conninfo_of("host=h dbname=d") == "host=h dbname=d");
  CHECK(libpq::conninfo_of("localhost:5432/app") ==
    "postgresql://localhost:5432/app");
  CHECK(libpq::conninfo_of("postgres://u@h/db?connect_timeout=2") ==
    "postgres://u@h/db?connect_timeout=2");
}

void test_libpq_conninfo_from_pieces() {
  CHECK(libpq::conninfo_of("localhost:5432/app", "joshua", "pwd") ==
    "host=localhost port=5432 dbname=app user=joshua password=pwd");

  // A piece nobody filled in is left out rather than sent as a blank one, so
  // the server's own default stands in for it.
  CHECK(libpq::conninfo_of("localhost", "", "") == "host=localhost");
  CHECK(libpq::conninfo_of("", "", "") == "");

  // A directory holding the socket is the host, and its name carries slashes.
  CHECK(libpq::conninfo_of("/var/run/postgresql/app", "j", "") ==
    "host=/var/run/postgresql dbname=app user=j");

  // A target spelled in libpq's own words keeps them and gains only the two
  // pieces a word list cannot carry inside a URI-free string.
  CHECK(libpq::conninfo_of("host=h port=1", "u", "p") ==
    "host=h port=1 user=u password=p");
}

void test_libpq_conninfo_quotes_a_value_that_needs_it() {
  // The separators of the grammar itself are what a value has to be quoted
  // against, and a quote inside it travels with a backslash on it.
  CHECK(libpq::conninfo_of("h", "it's", "") == "host=h user='it\\'s'");
  CHECK(libpq::conninfo_of("h", "", "a b") == "host=h password='a b'");
  CHECK(libpq::conninfo_of("h", "", "a\\b") == "host=h password='a\\\\b'");
  CHECK(libpq::conninfo_of("h", "plain", "plain") ==
    "host=h user=plain password=plain");
}

void test_libpq_open_needs_a_target() {
  // Nothing reaches libpq here: a session with no words to connect with is
  // refused by this library, which is the one case that needs no server.
  libpq::connection conn;
  CHECK(!conn.is_open());
  CHECK(conn.native() == nullptr);
  CHECK_THROWS(conn.open(""), backend::backend_error);
  CHECK_THROWS(conn.open("", "", ""), backend::backend_error);
  CHECK(!conn.is_open());
}

void test_libpq_connect_failure_keeps_the_password() {
  // Port 1 of this machine's own loopback answers with a refusal, so the
  // report a caller would log is written here rather than somewhere out of
  // reach -- and the credential half of the pieces never reaches it.
  libpq::connection conn;
  bool caught = false;
  try {
    conn.open("127.0.0.1:1/uniorm_absent", "nobody", "hunter2");
  } catch (backend::backend_error const& error) {
    caught = true;
    CHECK(error.backend_name() == "postgresql");
    std::string const text(error.what());
    CHECK(text.find("hunter2") == std::string::npos);
    CHECK(text.find("nobody") == std::string::npos);
    CHECK(!error.diagnostics().empty());
  }
  CHECK(caught);
  CHECK(!conn.is_open());
}

void test_libpq_error_is_backend_error() {
  // A SQL failure is classifiable through the public contract, and keeps the
  // fields the server reported under their own names.
  try {
    throw libpq::pq_error("execute", {
      { "42P01", 0, "relation \"nope\" does not exist" } });
  } catch (backend::backend_error const& error) {
    CHECK(error.backend_name() == "postgresql");
    CHECK(error.diagnostics().size() == 1);
    if (error.diagnostics().size() == 1) {
      CHECK(error.diagnostics()[0].state == "42P01");
      CHECK(error.diagnostics()[0].message ==
        "relation \"nope\" does not exist");
    }
    CHECK(std::string(error.what()).find(
      "[42P01] (0) relation \"nope\" does not exist") != std::string::npos);
  }
}

void test_libpq_error_reports_an_unnamed_failure() {
  // A failure the client named before the server ever answered has no records
  // to carry: the context says what was attempted and nothing is invented for
  // the report a caller reads.
  libpq::pq_error const failure("cancel", libpq::collect_diagnostics(
    static_cast<PGresult const*>(nullptr)));
  CHECK(failure.diagnostics().empty());

  bool caught = false;
  try {
    libpq::throw_if_failed(
      static_cast<PGresult const*>(nullptr), "fetch a row",
      { PGRES_COMMAND_OK });
  } catch (libpq::pq_error const& error) {
    caught = true;
    CHECK(std::string(error.what()).find("fetch a row") != std::string::npos);
    CHECK(error.diagnostics().empty());
  }
  CHECK(caught);
}
