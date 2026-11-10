#include "connection.hpp"

#include <cstdint>
#include <utility>

#include <uniorm/log.hpp>

namespace uniorm::odbc {

namespace {

SQLPOINTER attr_value(std::uintptr_t v) {
  return reinterpret_cast<SQLPOINTER>(v);
}

SQLCHAR* as_sql_chars(std::string_view s) {
  return reinterpret_cast<SQLCHAR*>(const_cast<char*>(s.data()));
}

}  // namespace

connection::connection(environment& env)
  : handle_(env.native(), "connection") {}

connection::~connection() {
  if (open_) {
    SQLRETURN rc = SQLDisconnect(native());
    if (!SQL_SUCCEEDED(rc)) {
      uniorm::detail::log(uniorm::log_level::warn, "odbc.connection", [&] {
        return "a connection left with a refused disconnect, so the driver may "
               "still hold the session" +
               diagnostic_text(SQL_HANDLE_DBC, native());
      });
    }
  }
}

connection::connection(connection&& other) noexcept
  : handle_(std::move(other.handle_)),
    open_(std::exchange(other.open_, false)) {}

connection& connection::operator=(connection&& other) noexcept {
  if (this != &other) {
    if (open_) {
      SQLRETURN rc = SQLDisconnect(native());
      if (!SQL_SUCCEEDED(rc)) {
        uniorm::detail::log(uniorm::log_level::warn, "odbc.connection", [&] {
          return "the connection a moved-to one replaced left with a refused "
                 "disconnect" +
                 diagnostic_text(SQL_HANDLE_DBC, native());
        });
      }
    }
    handle_ = std::move(other.handle_);
    open_ = std::exchange(other.open_, false);
  }
  return *this;
}

void connection::open(std::string_view connection_string) {
  if (open_) {
    throw odbc_error("connection is already open", {});
  }
  SQLCHAR completed[1024] = {};
  SQLSMALLINT completed_length = 0;
  SQLRETURN rc =
    SQLDriverConnect(native(), nullptr, as_sql_chars(connection_string),
      static_cast<SQLSMALLINT>(connection_string.size()), completed,
      static_cast<SQLSMALLINT>(sizeof(completed)), &completed_length,
      SQL_DRIVER_NOPROMPT);
  throw_if_error(rc, SQL_HANDLE_DBC, native(), "open connection");
  open_ = true;
}

void connection::close() {
  if (!open_) {
    return;
  }
  SQLRETURN rc = SQLDisconnect(native());
  throw_if_error(rc, SQL_HANDLE_DBC, native(), "close connection");
  open_ = false;
}

void connection::set_autocommit(bool enabled) {
  SQLRETURN rc = SQLSetConnectAttr(native(), SQL_ATTR_AUTOCOMMIT,
    attr_value(enabled ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF),
    SQL_IS_UINTEGER);
  throw_if_error(rc, SQL_HANDLE_DBC, native(), "set autocommit");
}

void connection::commit() {
  SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, native(), SQL_COMMIT);
  throw_if_error(rc, SQL_HANDLE_DBC, native(), "commit transaction");
}

void connection::rollback() {
  SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, native(), SQL_ROLLBACK);
  throw_if_error(rc, SQL_HANDLE_DBC, native(), "rollback transaction");
}

}  // namespace uniorm::odbc
