#pragma once

#include <utility>

#include <sql.h>
#include <sqlext.h>

#include <uniorm/odbc/error.hpp>

namespace uniorm::odbc::detail {

// Move-only RAII wrapper for an ODBC handle. Parent is SQL_NULL_HANDLE for
// environments, the owning environment for connections, and the owning
// connection for statements.
template <SQLSMALLINT HandleType>
class basic_handle {
public:
  explicit basic_handle(SQLHANDLE parent, char const* what) {
    constexpr SQLSMALLINT parent_type =
      HandleType == SQL_HANDLE_DBC ? SQL_HANDLE_ENV : SQL_HANDLE_DBC;
    SQLHANDLE h = null_handle();
    SQLRETURN rc = SQLAllocHandle(HandleType, parent, &h);
    if (!SQL_SUCCEEDED(rc)) {
      throw odbc_error(std::string("failed to allocate ODBC handle: ") + what,
        parent != null_handle() ? collect_diagnostics(parent_type, parent)
                                : std::vector<odbc_error::diagnostic>{});
    }
    h_ = h;
  }

  ~basic_handle() {
    if (h_ != null_handle()) {
      SQLFreeHandle(HandleType, h_);
    }
  }

  basic_handle(basic_handle&& other) noexcept
    : h_(std::exchange(other.h_, null_handle())) {}

  basic_handle& operator=(basic_handle&& other) noexcept {
    if (this != &other) {
      if (h_ != null_handle()) {
        SQLFreeHandle(HandleType, h_);
      }
      h_ = std::exchange(other.h_, null_handle());
    }
    return *this;
  }

  basic_handle(basic_handle const&) = delete;
  basic_handle& operator=(basic_handle const&) = delete;

  SQLHANDLE get() const noexcept {
    return h_;
  }
  explicit operator bool() const noexcept {
    return h_ != null_handle();
  }

private:
  static SQLHANDLE null_handle() noexcept {
    return nullptr;
  }

  SQLHANDLE h_ = nullptr;
};

using env_handle = basic_handle<SQL_HANDLE_ENV>;
using dbc_handle = basic_handle<SQL_HANDLE_DBC>;
using stmt_handle = basic_handle<SQL_HANDLE_STMT>;

}  // namespace uniorm::odbc::detail
