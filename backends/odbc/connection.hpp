#pragma once

// Private header: ODBC connection handle wrapper. Not installed.

#include <memory>
#include <string_view>

#include "environment.hpp"
#include "error.hpp"
#include "handles.hpp"

namespace uniorm::odbc {

// RAII wrapper for SQLHDBC. Move-only.
class UNIORM_ODBC_API connection {
public:
  explicit connection(std::shared_ptr<environment> env);
  ~connection();

  connection(connection&&) noexcept;
  connection& operator=(connection&&) noexcept;

  connection(connection const&) = delete;
  connection& operator=(connection const&) = delete;

  void open(std::string_view connection_string);
  void close();
  bool is_open() const noexcept {
    if (!open_) {
      return false;
    }
    // The driver's own dead flag, read locally with no round trip: it knows a
    // connection died once a call on it has failed. A driver without this
    // attribute falls back to the open flag rather than reporting false death.
    SQLUINTEGER dead = SQL_CD_FALSE;
    SQLRETURN rc = SQLGetConnectAttr(
      native(), SQL_ATTR_CONNECTION_DEAD, &dead, sizeof(dead), nullptr);
    if (!SQL_SUCCEEDED(rc)) {
      return open_;
    }
    return dead == SQL_CD_FALSE;
  }

  void set_autocommit(bool enabled);
  void commit();
  void rollback();

  SQLHDBC native() const noexcept {
    return static_cast<SQLHDBC>(handle_.get());
  }

private:
  std::shared_ptr<environment> env_;
  detail::dbc_handle handle_;
  bool open_ = false;
};

}  // namespace uniorm::odbc
