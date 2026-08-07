#include "uniorm/odbc/environment.hpp"

#include <cstdint>

namespace uniorm::odbc {

namespace {

SQLPOINTER attr_value(std::uintptr_t v) {
  return reinterpret_cast<SQLPOINTER>(v);
}

}  // namespace

environment::environment() : handle_(nullptr, "environment") {
  SQLRETURN rc = SQLSetEnvAttr(
    native(), SQL_ATTR_ODBC_VERSION, attr_value(SQL_OV_ODBC3_80), 0);
  if (rc == SQL_ERROR) {
    rc = SQLSetEnvAttr(
      native(), SQL_ATTR_ODBC_VERSION, attr_value(SQL_OV_ODBC3), 0);
  }
  throw_if_error(rc, SQL_HANDLE_ENV, native(), "set ODBC version");
}

environment::~environment() = default;

environment::environment(environment&&) noexcept = default;

environment& environment::operator=(environment&&) noexcept = default;

environment& shared_environment() {
  static environment env;
  return env;
}

}  // namespace uniorm::odbc
