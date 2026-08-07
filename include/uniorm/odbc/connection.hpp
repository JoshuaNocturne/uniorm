#pragma once

#include <string_view>

#include "detail/handles.hpp"
#include "environment.hpp"
#include "error.hpp"

namespace uniorm::odbc {

// RAII wrapper for SQLHDBC. Move-only.
class UNIORM_API connection {
public:
  explicit connection(environment& env);
  ~connection();

  connection(connection&&) noexcept;
  connection& operator=(connection&&) noexcept;

  connection(connection const&) = delete;
  connection& operator=(connection const&) = delete;

  void open(std::string_view connection_string);
  void open_dsn(
    std::string_view dsn, std::string_view user, std::string_view password);
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
