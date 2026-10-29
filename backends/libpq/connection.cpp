#include "connection.hpp"

#include <utility>

#include <uniorm/backend/error.hpp>

#include "error.hpp"
#include "handles.hpp"

namespace uniorm::libpq {
namespace {

// A connectioninfo is a word list, so a value holding a separator or a quote
// is quoted and its quotes and backslashes escaped -- the spelling libpq reads
// as those characters themselves.
void append_keyword(
  std::string& conninfo, char const* keyword, std::string_view value) {
  if (value.empty()) {
    return;
  }
  if (!conninfo.empty()) {
    conninfo.push_back(' ');
  }
  conninfo += keyword;
  conninfo.push_back('=');
  if (value.find_first_of(" \t'\\") == std::string_view::npos) {
    conninfo.append(value);
    return;
  }
  conninfo.push_back('\'');
  for (char const character : value) {
    if (character == '\'' || character == '\\') {
      conninfo.push_back('\\');
    }
    conninfo.push_back(character);
  }
  conninfo.push_back('\'');
}

// The target's host and port: a port is what follows the last colon, and
// nothing else in this grammar carries one.
void split_port(std::string_view& host, std::string_view& port) {
  std::size_t const colon = host.rfind(':');
  if (colon == std::string_view::npos) {
    return;
  }
  port = host.substr(colon + 1);
  host = host.substr(0, colon);
}

}  // namespace

std::string conninfo_of(std::string_view tail) {
  if (tail.empty()) {
    return {};
  }
  // A tail carrying a scheme or a keyword is already in one of libpq's own
  // grammars -- a URI with its query words, or a word list -- and is handed
  // over as it stands. Only a bare host needs the scheme put back on it.
  if (tail.find("://") != std::string_view::npos ||
      tail.find('=') != std::string_view::npos) {
    return std::string(tail);
  }
  return "postgresql://" + std::string(tail);
}

std::string conninfo_of(
  std::string_view target, std::string_view user, std::string_view password) {
  if (target.find('=') != std::string_view::npos) {
    // The target names the server in libpq's own words already, so only the
    // two pieces it cannot carry have to be added.
    std::string conninfo(target);
    append_keyword(conninfo, "user", user);
    append_keyword(conninfo, "password", password);
    return conninfo;
  }
  std::string_view host = target;
  std::string_view port;
  std::string_view database;
  std::size_t const slash = target.rfind('/');
  if (slash != std::string_view::npos) {
    host = target.substr(0, slash);
    database = target.substr(slash + 1);
  }
  split_port(host, port);
  std::string conninfo;
  append_keyword(conninfo, "host", host);
  append_keyword(conninfo, "port", port);
  append_keyword(conninfo, "dbname", database);
  append_keyword(conninfo, "user", user);
  append_keyword(conninfo, "password", password);
  return conninfo;
}

connection::~connection() {
  close();
}

connection::connection(connection&& other) noexcept
  : conn_(std::exchange(other.conn_, nullptr)),
    autocommit_(other.autocommit_),
    transaction_open_(other.transaction_open_) {}

connection& connection::operator=(connection&& other) noexcept {
  if (this != &other) {
    close();
    conn_ = std::exchange(other.conn_, nullptr);
    autocommit_ = other.autocommit_;
    transaction_open_ = other.transaction_open_;
  }
  return *this;
}

void connection::connect(std::string const& conninfo) {
  if (conninfo.empty()) {
    throw backend::backend_error("postgresql",
      "a PostgreSQL session needs a target, a word list or a URI", {});
  }
  // The blocking form: it either answers or fails, and a session that may
  // never be handed back would leave a caller waiting with nothing to close.
  PGconn* const made = PQconnectdb(conninfo.c_str());
  if (PQstatus(made) != CONNECTION_OK) {
    std::vector<pq_error::diagnostic> const diags = collect_diagnostics(made);
    PQfinish(made);
    // No conninfo in what is reported: it can carry a password, and libpq's
    // own failure names the host and port it could not reach.
    throw pq_error("connect to the PostgreSQL server", diags);
  }
  // The value text this backend reads is taken for ISO-shaped and hex-bytes:
  // a date written the other way carries the day where the month was, and an
  // escape-form byte string is no hex to decode. Pinned before the session is
  // taken on, so a refusal leaves no connection half-read behind.
  detail::result_holder shaped{ PQexec(made,
    "SET DateStyle = 'ISO, MDY'; SET bytea_output = 'hex'") };
  if (PQresultStatus(shaped.get()) != PGRES_COMMAND_OK) {
    std::vector<pq_error::diagnostic> const diags =
      collect_diagnostics(shaped.get());
    PQfinish(made);
    throw pq_error("set the shapes a session writes its values in", diags);
  }
  close();
  conn_ = made;
  transaction_open_ = PQtransactionStatus(made) == PQTRANS_INTRANS ||
    PQtransactionStatus(made) == PQTRANS_INERROR;
}

void connection::open(std::string_view tail) {
  connect(conninfo_of(tail));
}

void connection::open(
  std::string_view target, std::string_view user, std::string_view password) {
  connect(conninfo_of(target, user, password));
}

void connection::close() {
  if (conn_ == nullptr) {
    return;
  }
  PGconn* const session = std::exchange(conn_, nullptr);
  transaction_open_ = false;
  PQfinish(session);
}

bool connection::is_open() const noexcept {
  return conn_ != nullptr && PQstatus(conn_) == CONNECTION_OK;
}

void connection::set_autocommit(bool enabled) {
  if (enabled && !autocommit_) {
    // A manual-mode session has no commit coming on its own: the switch is
    // what ends the transaction its pending work sits in.
    commit();
  }
  autocommit_ = enabled;
}

void connection::open_transaction() {
  if (autocommit_ || transaction_open_ || conn_ == nullptr) {
    return;
  }
  run_transaction_command("BEGIN", "begin transaction");
  transaction_open_ = true;
}

void connection::commit() {
  if (!transaction_open_) {
    return;
  }
  run_transaction_command("COMMIT", "commit transaction");
  transaction_open_ = false;
}

void connection::rollback() {
  if (!transaction_open_) {
    // The server is already where a roll back would put it: after a failure
    // it aborts the transaction itself, and asking again costs a round trip
    // for a statement that changes nothing.
    if (conn_ != nullptr &&
        PQtransactionStatus(conn_) == PQTRANS_INERROR) {
      run_transaction_command("ROLLBACK", "roll back transaction");
    }
    return;
  }
  run_transaction_command("ROLLBACK", "roll back transaction");
  transaction_open_ = false;
}

void connection::run_transaction_command(
  char const* sql, std::string const& context) {
  detail::result_holder answer{ PQexec(conn_, sql) };
  throw_if_failed(answer.get(), context, { PGRES_COMMAND_OK });
}

}  // namespace uniorm::libpq
