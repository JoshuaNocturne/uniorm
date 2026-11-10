#include "error.hpp"

#include <string>
#include <vector>

#include <uniorm/log.hpp>

namespace uniorm::odbc {

odbc_error::odbc_error(
  std::string const& context, std::vector<diagnostic> diags)
  : backend::backend_error("odbc", context, std::move(diags)) {}

std::vector<odbc_error::diagnostic> collect_diagnostics(
  SQLSMALLINT handle_type, SQLHANDLE handle) {
  std::vector<odbc_error::diagnostic> result;
  for (SQLSMALLINT record = 1; record <= 64; ++record) {
    SQLCHAR state[6] = {};
    SQLINTEGER native_code = 0;
    SQLCHAR message[1024] = {};
    SQLSMALLINT message_length = 0;
    SQLRETURN rc =
      SQLGetDiagRec(handle_type, handle, record, state, &native_code, message,
        static_cast<SQLSMALLINT>(sizeof(message)), &message_length);
    if (rc == SQL_NO_DATA || !SQL_SUCCEEDED(rc)) {
      break;
    }
    odbc_error::diagnostic d;
    d.state.assign(reinterpret_cast<char const*>(state));
    d.native_code = native_code;
    d.message.assign(reinterpret_cast<char const*>(message), message_length);
    result.push_back(std::move(d));
  }
  return result;
}

std::string diagnostic_text(SQLSMALLINT handle_type, SQLHANDLE handle) {
  std::string text;
  for (auto const& record : collect_diagnostics(handle_type, handle)) {
    text += ": [" + record.state + "] (" + std::to_string(record.native_code) +
            ") " + record.message;
  }
  return text;
}

void throw_if_error(SQLRETURN rc, SQLSMALLINT handle_type, SQLHANDLE handle,
  std::string const& context, bool tolerate_no_data) {
  if (rc == SQL_SUCCESS_WITH_INFO) {
    uniorm::detail::log(uniorm::log_level::info, "odbc.diagnostics", [&] {
      return context + " answered with a warning" +
             diagnostic_text(handle_type, handle);
    });
    return;
  }
  if (rc == SQL_SUCCESS) {
    return;
  }
  if (tolerate_no_data && rc == SQL_NO_DATA) {
    return;
  }
  throw odbc_error(context, collect_diagnostics(handle_type, handle));
}

}  // namespace uniorm::odbc
