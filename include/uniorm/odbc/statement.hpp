#pragma once

#include <cstddef>
#include <string_view>

#include "connection.hpp"
#include "detail/handles.hpp"
#include "error.hpp"

namespace uniorm::odbc {

// RAII wrapper for SQLHSTMT. Move-only. Exposes raw bind primitives; the
// typed binding layer above wraps these.
class UNIORM_API statement {
public:
  explicit statement(connection& conn);
  ~statement();

  statement(statement&&) noexcept;
  statement& operator=(statement&&) noexcept;

  statement(statement const&) = delete;
  statement& operator=(statement const&) = delete;

  void prepare(std::string_view sql);
  void execute();
  bool fetch();  // false when the result set is exhausted
  std::size_t affected_rows() const;
  std::size_t column_count() const;

  // index is 1-based. indicator points to a SQLLEN owned by the caller
  // that must outlive the statement execution.
  void bind_parameter(SQLUSMALLINT index, SQLSMALLINT c_type,
    SQLSMALLINT sql_type, SQLPOINTER value, SQLLEN buffer_length,
    SQLLEN* indicator, SQLULEN column_size = 0, SQLSMALLINT decimal_digits = 0);
  void bind_column(SQLUSMALLINT index, SQLSMALLINT c_type, SQLPOINTER value,
    SQLLEN buffer_length, SQLLEN* indicator);

  void close_cursor();
  void reset();  // unbind columns and reset parameters

  SQLHSTMT native() const noexcept {
    return static_cast<SQLHSTMT>(handle_.get());
  }

private:
  detail::stmt_handle handle_;
};

}  // namespace uniorm::odbc
