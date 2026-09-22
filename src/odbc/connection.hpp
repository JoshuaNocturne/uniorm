#pragma once

// Private header: ODBC connection handle wrapper. Not installed.

#include <string_view>

#include "handles.hpp"
#include "environment.hpp"
#include "error.hpp"

namespace uniorm::odbc {

// RAII wrapper for SQLHDBC. Move-only.
class UNIORM_ODBC_API connection {
public:
  explicit connection(environment& env);
  ~connection();

  connection(connection&&) noexcept;
  connection& operator=(connection&&) noexcept;

  connection(connection const&) = delete;
  connection& operator=(connection const&) = delete;

  void open(std::string_view connection_string);
  void close();
  bool is_open() const noexcept {
    return open_;
  }

  void set_autocommit(bool enabled);
  void commit();
  void rollback();

  SQLHDBC native() const noexcept {
    return static_cast<SQLHDBC>(handle_.get());
  }

private:
  detail::dbc_handle handle_;
  bool open_ = false;
};

}  // namespace uniorm::odbc
