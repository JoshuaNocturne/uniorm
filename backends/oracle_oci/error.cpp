#include "error.hpp"

#include <cstddef>
#include <cstring>

namespace uniorm::oracle_oci {

oci_error::oci_error(
  std::string const& context, std::vector<diagnostic> diags)
  : backend::backend_error("oracle", context, std::move(diags)) {}

namespace {

// "ORA-00942: table or view does not exist" leads with its code; anything
// before the first colon that is short and carries a dash is that code, and
// the rest is prose. A message without one keeps an empty state rather than
// borrowing a word from the prose as a code.
std::string leading_code(std::string const& message) {
  std::size_t const colon = message.find(':');
  if (colon == std::string_view::npos || colon > 12) {
    return {};
  }
  if (message.find('-') > colon) {
    return {};
  }
  return message.substr(0, colon);
}

}  // namespace

std::vector<oci_error::diagnostic> collect_diagnostics(OCIError* error) {
  std::vector<oci_error::diagnostic> result;
  if (error == nullptr) {
    return result;
  }
  for (ub4 record = 1; record <= 32; ++record) {
    sb4 code = 0;
    OraText message[1024] = {};
    sword const status = OCIErrorGet(error, record, nullptr, &code, message,
      static_cast<ub4>(sizeof(message)), OCI_HTYPE_ERROR);
    if (status != OCI_SUCCESS) {
      break;
    }
    char const* const begin = reinterpret_cast<char const*>(message);
    oci_error::diagnostic entry;
    entry.message.assign(begin, std::strlen(begin));
    entry.native_code = code;
    entry.state = leading_code(entry.message);
    result.push_back(std::move(entry));
  }
  return result;
}

void throw_if_error(sword status, OCIError* error,
  std::string const& context, sword tolerated) {
  if (status == OCI_SUCCESS || status == OCI_SUCCESS_WITH_INFO) {
    return;
  }
  if (status == tolerated) {
    return;
  }
  throw oci_error(context, collect_diagnostics(error));
}

}  // namespace uniorm::oracle_oci
