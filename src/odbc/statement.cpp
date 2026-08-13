#include "uniorm/odbc/statement.hpp"

namespace uniorm::odbc {

statement::statement(connection& conn) : handle_(conn.native(), "statement") {}

statement::~statement() = default;

statement::statement(statement&&) noexcept = default;

statement& statement::operator=(statement&&) noexcept = default;

void statement::prepare(std::string_view sql) {
  SQLRETURN rc = SQLPrepare(native(),
    reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.data())),
    static_cast<SQLINTEGER>(sql.size()));
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "prepare statement");
}

void statement::execute() {
  SQLRETURN rc = SQLExecute(native());
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "execute statement",
    /*tolerate_no_data=*/true);
}

bool statement::fetch() {
  SQLRETURN rc = SQLFetch(native());
  if (rc == SQL_NO_DATA) {
    return false;
  }
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "fetch row");
  return true;
}

std::size_t statement::affected_rows() const {
  SQLLEN count = 0;
  SQLRETURN rc = SQLRowCount(native(), &count);
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "query affected row count");
  return count < 0 ? 0 : static_cast<std::size_t>(count);
}

std::size_t statement::column_count() const {
  SQLSMALLINT count = 0;
  SQLRETURN rc = SQLNumResultCols(native(), &count);
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "query result column count");
  return static_cast<std::size_t>(count);
}

void statement::bind_parameter(SQLUSMALLINT index, SQLSMALLINT c_type,
  SQLSMALLINT sql_type, SQLPOINTER value, SQLLEN buffer_length,
  SQLLEN* indicator, SQLULEN column_size, SQLSMALLINT decimal_digits) {
  SQLRETURN rc = SQLBindParameter(native(), index, SQL_PARAM_INPUT, c_type,
    sql_type, column_size, decimal_digits, value, buffer_length, indicator);
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "bind parameter");
}

void statement::bind_column(SQLUSMALLINT index, SQLSMALLINT c_type,
  SQLPOINTER value, SQLLEN buffer_length, SQLLEN* indicator) {
  SQLRETURN rc =
    SQLBindCol(native(), index, c_type, value, buffer_length, indicator);
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "bind column");
}

void statement::close_cursor() {
  SQLRETURN rc = SQLCloseCursor(native());
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "close cursor");
}

void statement::reset() {
  // SQL_CLOSE (unlike SQLCloseCursor) is a no-op when no cursor is open,
  // which makes reset() safe on statements reused from the cache.
  SQLRETURN rc = SQLFreeStmt(native(), SQL_CLOSE);
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "close statement cursor");
  rc = SQLFreeStmt(native(), SQL_RESET_PARAMS);
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "reset statement parameters");
  rc = SQLFreeStmt(native(), SQL_UNBIND);
  throw_if_error(rc, SQL_HANDLE_STMT, native(), "unbind statement columns");
}

}  // namespace uniorm::odbc
