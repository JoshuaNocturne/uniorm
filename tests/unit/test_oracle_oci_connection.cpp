#include <string>

#include <uniorm/backend/error.hpp>

#include "oracle_oci/connection.hpp"
#include "oracle_oci/error.hpp"

#include "check.hpp"

using namespace uniorm;

void test_oracle_oci_logon_needs_a_target() {
  // An empty connect piece is refused before OCI is asked, so this case
  // reaches the backend's own diagnostic without a server.
  oracle_oci::connection conn;
  CHECK(!conn.is_open());
  CHECK(conn.service() == nullptr);
  CHECK_THROWS(conn.open(""), backend::backend_error);
  CHECK_THROWS(conn.open("scott/tiger@"), backend::backend_error);
}

void test_oracle_oci_logon_splits_credentials() {
  // A closed port answers with a diagnostic of its own, and the context it
  // lands in names only the connect piece. What reached OCILogon2 therefore
  // says where the credential half was cut, and that no part of it travelled
  // into the message a caller logs.
  oracle_oci::connection conn;
  bool caught = false;
  try {
    conn.open("scott/secret@127.0.0.1:1/orcl");
  } catch (backend::backend_error const& error) {
    caught = true;
    CHECK(error.backend_name() == "oracle");
    std::string const text(error.what());
    CHECK(text.find("127.0.0.1:1/orcl") != std::string::npos);
    CHECK(text.find("secret") == std::string::npos);
    CHECK(text.find("scott") == std::string::npos);
  }
  CHECK(caught);
  CHECK(!conn.is_open());
}

void test_oracle_oci_logon_cuts_at_the_last_marker() {
  // A connect descriptor holds no '@' while a password may, so the last one
  // is the divider: everything before it is credential, everything after is
  // the target.
  oracle_oci::connection conn;
  bool caught = false;
  try {
    conn.open("ma#il@ho/st@127.0.0.1:2/orcl");
  } catch (backend::backend_error const& error) {
    caught = true;
    std::string const text(error.what());
    CHECK(text.find("log on to 127.0.0.1:2/orcl") != std::string::npos);
    CHECK(text.find("ma#il") == std::string::npos);
  }
  CHECK(caught);
}

void test_oracle_oci_logon_without_credentials() {
  // No '@' at all means no credentials, which the server's external
  // authentication answers for; the whole string is the target.
  oracle_oci::connection conn;
  bool caught = false;
  try {
    conn.open("127.0.0.1:3/orcl");
  } catch (backend::backend_error const& error) {
    caught = true;
    CHECK(error.backend_name() == "oracle");
    CHECK(std::string(error.what()).find(
      "log on to 127.0.0.1:3/orcl") != std::string::npos);
  }
  CHECK(caught);
}

void test_oracle_oci_error_is_backend_error() {
  // A SQL failure is classifiable through the public contract, and keeps the
  // records the handle carried. OCIErrorGet serves no SQL state, so the state
  // an entry carries is the ORA- code leading its message.
  try {
    throw oracle_oci::oci_error("execute", {
      { "ORA-00942", 942, "table or view does not exist" } });
  } catch (backend::backend_error const& error) {
    CHECK(error.backend_name() == "oracle");
    CHECK(error.diagnostics().size() == 1);
    if (error.diagnostics().size() == 1) {
      CHECK(error.diagnostics()[0].state == "ORA-00942");
      CHECK(error.diagnostics()[0].native_code == 942);
    }
    CHECK(std::string(error.what()).find(
      "[ORA-00942] (942) table or view does not exist") != std::string::npos);
  }
}

void test_oracle_oci_error_reports_a_tolerated_code() {
  // A fetch that ran out and a LOB read that wants another piece are both
  // reported as returns, not failures; only the call that expects them has
  // that exemption.
  bool caught = false;
  try {
    oracle_oci::throw_if_error(OCI_NO_DATA, nullptr, "fetch a row");
  } catch (oracle_oci::oci_error const& error) {
    caught = true;
    CHECK(std::string(error.what()).find("fetch a row") != std::string::npos);
    CHECK(error.diagnostics().empty());
  }
  CHECK(caught);

  oracle_oci::throw_if_error(OCI_NO_DATA, nullptr, "fetch a row", OCI_NO_DATA);
  oracle_oci::throw_if_error(OCI_SUCCESS, nullptr, "fetch a row");
  oracle_oci::throw_if_error(
    OCI_SUCCESS_WITH_INFO, nullptr, "fetch a row");
}
