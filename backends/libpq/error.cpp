#include "error.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace uniorm::libpq {
namespace {

char const* field_of(PGresult const* result, char field) {
  char const* const text = PQresultErrorField(result, field);
  return text == nullptr ? "" : text;
}

void add_message(std::vector<pq_error::diagnostic>& out, std::string state,
  std::string_view message) {
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r')) {
    message.remove_suffix(1);
  }
  if (message.empty()) {
    return;
  }
  out.push_back({ std::move(state), 0, std::string(message) });
}

// The status word, for the one failure that leaves no message behind: a
// cancelled statement reports its status and nothing else.
char const* status_name(ExecStatusType status) {
  switch (status) {
  case PGRES_EMPTY_QUERY:
    return "empty query";
  case PGRES_COMMAND_OK:
    return "command completed";
  case PGRES_TUPLES_OK:
    return "rows returned";
  case PGRES_COPY_OUT:
    return "copy out started";
  case PGRES_COPY_IN:
    return "copy in started";
  case PGRES_COPY_BOTH:
    return "copy started both ways";
  case PGRES_SINGLE_TUPLE:
    return "one row returned";
  case PGRES_NONFATAL_ERROR:
    return "warning";
  case PGRES_BAD_RESPONSE:
    return "unusable server response";
  case PGRES_FATAL_ERROR:
    return "fatal error";
  default:
    return "no status";
  }
}

}  // namespace

pq_error::pq_error(std::string const& context, std::vector<diagnostic> diags)
  : backend_error("postgresql", context, std::move(diags)) {}

std::vector<pq_error::diagnostic> collect_diagnostics(
  PGresult const* result) {
  std::vector<pq_error::diagnostic> out;
  if (result == nullptr) {
    return out;
  }
  // The server sends a failure as a set of named fields rather than one line,
  // so each becomes a record of its own and the state is shared by them all.
  std::string const state = field_of(result, PG_DIAG_SQLSTATE);
  add_message(out, state, field_of(result, PG_DIAG_MESSAGE_PRIMARY));
  add_message(out, state, field_of(result, PG_DIAG_MESSAGE_DETAIL));
  add_message(out, state, field_of(result, PG_DIAG_MESSAGE_HINT));
  if (out.empty()) {
    add_message(out, state, status_name(PQresultStatus(result)));
  }
  return out;
}

std::vector<pq_error::diagnostic> collect_diagnostics(
  PGconn const* connection) {
  std::vector<pq_error::diagnostic> out;
  if (connection == nullptr) {
    return out;
  }
  // PQerrorMessage keeps every line the attempt wrote, and a failed name
  // lookup is usually two of them.
  std::string const report = PQerrorMessage(connection);
  std::size_t line = 0;
  while (line < report.size()) {
    std::size_t const end = report.find('\n', line);
    std::size_t const stop = end == std::string::npos ? report.size() : end;
    add_message(out, "", report.substr(line, stop - line));
    line = stop + 1;
  }
  if (out.empty()) {
    add_message(out, "", "the connection was refused");
  }
  return out;
}

void throw_if_failed(PGresult const* result, std::string const& context,
  std::initializer_list<ExecStatusType> ok) {
  ExecStatusType const status = PQresultStatus(result);
  if (std::find(ok.begin(), ok.end(), status) != ok.end()) {
    return;
  }
  throw pq_error(context, collect_diagnostics(result));
}

}  // namespace uniorm::libpq
