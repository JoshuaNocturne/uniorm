#include "schema_catalog.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include <sql.h>
#include <sqlext.h>

#include "error.hpp"
#include "native_types.hpp"
#include "statement.hpp"

namespace uniorm::odbc {

namespace {

using meta = schema_meta;

// A catalog pattern. An empty string asks for every value, which the
// driver spells as a null pointer, not a zero-length one.
struct text_arg {
  SQLCHAR* ptr = nullptr;
  SQLSMALLINT len = 0;

  explicit text_arg(std::string const& s) {
    if (!s.empty()) {
      ptr = reinterpret_cast<SQLCHAR*>(const_cast<char*>(s.c_str()));
      len = SQL_NTS;
    }
  }
};

struct char_buffer {
  char data[1024] = {};
  SQLLEN ind = SQL_NULL_DATA;

  bool is_null() const noexcept {
    return ind == SQL_NULL_DATA;
  }

  std::string str() const {
    if (ind <= 0 || ind == SQL_NULL_DATA) {
      return {};
    }
    return std::string(
      data, static_cast<std::size_t>(std::min<SQLLEN>(ind, sizeof(data) - 1)));
  }
};

// A catalog name. SQLGetInfo reports the length it wrote itself, so there
// is no indicator to consult, and the buffer may have been the limit.
std::string info_text(SQLHDBC dbc, SQLUSMALLINT info) {
  char data[1024] = {};
  SQLSMALLINT length = 0;
  if (!SQL_SUCCEEDED(
        SQLGetInfo(dbc, info, data, sizeof(data), &length)) ||
      length <= 0) {
    return {};
  }
  return std::string(
    data, static_cast<std::size_t>(
      std::min<SQLSMALLINT>(length, sizeof(data) - 1)));
}

}  // namespace

std::string odbc_schema_meta::database_name() {
  return info_text(conn_.native(), SQL_DATABASE_NAME);
}

std::vector<meta::table_row> odbc_schema_meta::tables(
  std::string_view catalog, std::string_view schema) {
  std::string catalog_pattern(catalog);
  std::string schema_pattern(schema);
  text_arg cat(catalog_pattern);
  text_arg sch(schema_pattern);

  odbc::statement stmt(conn_);
  SQLRETURN rc = SQLTables(stmt.native(), cat.ptr, cat.len, sch.ptr, sch.len,
    nullptr, 0, nullptr, 0);
  odbc::throw_if_error(rc, SQL_HANDLE_STMT, stmt.native(), "SQLTables");

  char_buffer table_catalog;
  char_buffer table_schema;
  char_buffer table_name;
  char_buffer table_type;
  stmt.bind_column(
    1, SQL_C_CHAR, table_catalog.data, sizeof(table_catalog.data),
    &table_catalog.ind);
  stmt.bind_column(
    2, SQL_C_CHAR, table_schema.data, sizeof(table_schema.data),
    &table_schema.ind);
  stmt.bind_column(
    3, SQL_C_CHAR, table_name.data, sizeof(table_name.data),
    &table_name.ind);
  stmt.bind_column(
    4, SQL_C_CHAR, table_type.data, sizeof(table_type.data),
    &table_type.ind);

  std::vector<table_row> rows;
  while (stmt.fetch()) {
    // ODBC's word for a plain table, and the word some drivers use for
    // it; everything else in the result is a view or a system object.
    std::string type = table_type.str();
    if (type != "TABLE" && type != "BASE TABLE") {
      continue;
    }
    rows.push_back(
      table_row{ table_catalog.str(), table_schema.str(), table_name.str() });
  }
  return rows;
}

std::vector<meta::column_row> odbc_schema_meta::table_columns(
  table_ref const& table) {
  std::string cat(table.catalog);
  std::string sch(table.schema);
  std::string tbl(table.name);
  text_arg cat_arg(cat);
  text_arg sch_arg(sch);
  text_arg tbl_arg(tbl);

  odbc::statement stmt(conn_);
  SQLRETURN rc = SQLColumns(stmt.native(), cat_arg.ptr, cat_arg.len,
    sch_arg.ptr, sch_arg.len, tbl_arg.ptr, tbl_arg.len, nullptr, 0);
  odbc::throw_if_error(
    rc, SQL_HANDLE_STMT, stmt.native(), "SQLColumns(" + tbl + ")");

  char_buffer name;
  char_buffer type_name;
  char_buffer default_value;
  SQLINTEGER data_type = 0;
  SQLLEN data_type_ind = SQL_NULL_DATA;
  SQLINTEGER column_size = 0;
  SQLLEN size_ind = SQL_NULL_DATA;
  SQLSMALLINT decimals = 0;
  SQLLEN decimals_ind = SQL_NULL_DATA;
  SQLSMALLINT nullable = SQL_NO_NULLS;
  SQLLEN nullable_ind = SQL_NULL_DATA;

  stmt.bind_column(4, SQL_C_CHAR, name.data, sizeof(name.data), &name.ind);
  stmt.bind_column(
    5, SQL_C_SLONG, &data_type, sizeof(data_type), &data_type_ind);
  stmt.bind_column(
    6, SQL_C_CHAR, type_name.data, sizeof(type_name.data), &type_name.ind);
  stmt.bind_column(
    7, SQL_C_SLONG, &column_size, sizeof(column_size), &size_ind);
  stmt.bind_column(
    9, SQL_C_SSHORT, &decimals, sizeof(decimals), &decimals_ind);
  stmt.bind_column(
    11, SQL_C_SSHORT, &nullable, sizeof(nullable), &nullable_ind);
  stmt.bind_column(13, SQL_C_CHAR, default_value.data,
    sizeof(default_value.data), &default_value.ind);

  std::vector<column_row> rows;
  while (stmt.fetch()) {
    if (name.is_null()) {
      continue;
    }
    column_row row;
    row.shape.name = name.str();
    row.shape.type = data_type_ind == SQL_NULL_DATA
                       ? sql_type::other
                       : sql_type_from_native(data_type);
    row.type_name = type_name.str();
    row.size = size_ind == SQL_NULL_DATA ? 0 : column_size;
    row.decimals = decimals_ind == SQL_NULL_DATA ? 0 : decimals;
    row.shape.nullable =
      nullable_ind != SQL_NULL_DATA && nullable != SQL_NO_NULLS;
    if (default_value.ind != SQL_NULL_DATA) {
      row.default_value = default_value.str();
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<std::string> odbc_schema_meta::primary_key(
  table_ref const& table) {
  std::string cat(table.catalog);
  std::string sch(table.schema);
  std::string tbl(table.name);
  text_arg cat_arg(cat);
  text_arg sch_arg(sch);
  text_arg tbl_arg(tbl);

  odbc::statement stmt(conn_);
  SQLRETURN rc = SQLPrimaryKeys(stmt.native(), cat_arg.ptr, cat_arg.len,
    sch_arg.ptr, sch_arg.len, tbl_arg.ptr, tbl_arg.len);
  odbc::throw_if_error(
    rc, SQL_HANDLE_STMT, stmt.native(), "SQLPrimaryKeys(" + tbl + ")");

  char_buffer name;
  stmt.bind_column(4, SQL_C_CHAR, name.data, sizeof(name.data), &name.ind);

  std::vector<std::string> columns;
  while (stmt.fetch()) {
    if (!name.is_null()) {
      columns.push_back(name.str());
    }
  }
  return columns;
}

std::vector<meta::foreign_key_row> odbc_schema_meta::foreign_keys(
  table_ref const& table) {
  std::string cat(table.catalog);
  std::string sch(table.schema);
  std::string tbl(table.name);
  text_arg cat_arg(cat);
  text_arg sch_arg(sch);
  text_arg tbl_arg(tbl);

  odbc::statement stmt(conn_);
  // The primary-key side stays wide open: the rows asked for are the keys
  // this table owns, wherever they point.
  SQLRETURN rc = SQLForeignKeys(stmt.native(), nullptr, 0, nullptr, 0, nullptr,
    0, cat_arg.ptr, cat_arg.len, sch_arg.ptr, sch_arg.len, tbl_arg.ptr,
    tbl_arg.len);
  odbc::throw_if_error(
    rc, SQL_HANDLE_STMT, stmt.native(), "SQLForeignKeys(" + tbl + ")");

  char_buffer pk_table;
  char_buffer pk_column;
  char_buffer fk_column;
  stmt.bind_column(
    3, SQL_C_CHAR, pk_table.data, sizeof(pk_table.data), &pk_table.ind);
  stmt.bind_column(
    4, SQL_C_CHAR, pk_column.data, sizeof(pk_column.data), &pk_column.ind);
  stmt.bind_column(
    8, SQL_C_CHAR, fk_column.data, sizeof(fk_column.data), &fk_column.ind);

  std::vector<foreign_key_row> rows;
  while (stmt.fetch()) {
    rows.push_back(foreign_key_row{
      pk_table.str(), pk_column.str(), fk_column.str() });
  }
  return rows;
}

std::vector<meta::index_row> odbc_schema_meta::indexes(
  table_ref const& table) {
  std::string cat(table.catalog);
  std::string sch(table.schema);
  std::string tbl(table.name);
  text_arg cat_arg(cat);
  text_arg sch_arg(sch);
  text_arg tbl_arg(tbl);

  odbc::statement stmt(conn_);
  SQLRETURN rc = SQLStatistics(stmt.native(), cat_arg.ptr, cat_arg.len,
    sch_arg.ptr, sch_arg.len, tbl_arg.ptr, tbl_arg.len, SQL_INDEX_ALL,
    SQL_QUICK);
  odbc::throw_if_error(
    rc, SQL_HANDLE_STMT, stmt.native(), "SQLStatistics(" + tbl + ")");

  SQLSMALLINT non_unique = 0;
  SQLLEN non_unique_ind = SQL_NULL_DATA;
  char_buffer index_name;
  SQLSMALLINT row_type = 0;
  SQLLEN row_type_ind = SQL_NULL_DATA;
  char_buffer column;

  stmt.bind_column(
    4, SQL_C_SSHORT, &non_unique, sizeof(non_unique), &non_unique_ind);
  stmt.bind_column(
    6, SQL_C_CHAR, index_name.data, sizeof(index_name.data), &index_name.ind);
  stmt.bind_column(
    7, SQL_C_SSHORT, &row_type, sizeof(row_type), &row_type_ind);
  stmt.bind_column(
    9, SQL_C_CHAR, column.data, sizeof(column.data), &column.ind);

  std::vector<index_row> rows;
  while (stmt.fetch()) {
    // TYPE names what the row describes: only the plain index rows are
    // indexes, the rest are the table-statistics pseudo row and the
    // clustered or hash accesses an index may also enable.
    if (row_type_ind == SQL_NULL_DATA || row_type != SQL_INDEX_OTHER) {
      continue;
    }
    std::string iname = index_name.str();
    if (iname.empty()) {
      continue;
    }
    auto it = std::find_if(rows.begin(), rows.end(),
      [&](index_row const& row) { return row.name == iname; });
    if (it == rows.end()) {
      rows.push_back(index_row{ iname, {}, false });
      it = std::prev(rows.end());
    }
    it->unique = non_unique_ind != SQL_NULL_DATA && non_unique == 0;
    it->columns.push_back(column.str());
  }
  return rows;
}

}  // namespace uniorm::odbc
