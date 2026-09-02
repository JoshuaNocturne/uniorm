#pragma once

// Private header: ODBC statement handle wrapper. Not installed.

#include <cstddef>
#include <string_view>

#include "connection.hpp"
#include "handles.hpp"
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

  // Block fetch support: set the number of rows to fetch per SQLFetch call.
  void set_row_array_size(SQLULEN size);
  SQLULEN rows_fetched() const noexcept { return rows_fetched_; }

  // Batch insert support: set the number of parameter sets per SQLExecute.
  void set_paramset_size(SQLULEN size);

  // index is 1-based. indicator points to a SQLLEN owned by the caller
  // that must outlive the statement execution.
  void bind_parameter(SQLUSMALLINT index, SQLSMALLINT c_type,
    SQLSMALLINT sql_type, SQLPOINTER value, SQLLEN buffer_length,
    SQLLEN* indicator, SQLULEN column_size = 0, SQLSMALLINT decimal_digits = 0);
  void bind_column(SQLUSMALLINT index, SQLSMALLINT c_type, SQLPOINTER value,
    SQLLEN buffer_length, SQLLEN* indicator);

  void close_cursor();
  // Return the statement to a fresh state for reuse: close any open
  // cursor, unbind columns, reset parameters.
  void reset();

  SQLHSTMT native() const noexcept {
    return static_cast<SQLHSTMT>(handle_.get());
  }

private:
  detail::stmt_handle handle_;
  SQLULEN row_array_size_ = 1;
  SQLULEN rows_fetched_ = 0;
  SQLULEN paramset_size_ = 1;
};

}  // namespace uniorm::odbc
