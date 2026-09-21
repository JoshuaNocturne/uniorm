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
using table_row = meta::table_row;
using column_row = meta::column_row;

struct text_arg {
  SQLCHAR* ptr = nullptr;
  SQLSMALLINT len = 0;

  // An empty text is passed as the ODBC "unrestricted" pointer in every
  // mode; a literal empty string on SchemaName means "tables without an
  // owner" to MySQL and MariaDB, which answers with nothing there. The
  // exact-mode re-filter enforces byte equality afterwards.
  explicit text_arg(std::string const& text, bool exact = false) {
    if (!text.empty()) {
      ptr = reinterpret_cast<SQLCHAR*>(const_cast<char*>(text.c_str()));
      len = exact ? exact_catalog_arg_length(text) : SQL_NTS;
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

  std::string exact_str(
    std::string_view field, bool allow_empty = false) const {
    return exact_catalog_text({ data, sizeof(data) }, ind, field, allow_empty);
  }
};

// The three name columns a catalog result leads its rows with, bound at a
// column offset: 1 for SQLTables, SQLColumns, SQLPrimaryKeys and
// SQLStatistics; 5 for the foreign-key table of SQLForeignKeys. A row whose
// names are not the ones asked for describes another object.
struct reported_names {
  char_buffer catalog;
  char_buffer schema;
  char_buffer table;

  void bind(odbc::statement& stmt, SQLUSMALLINT first) {
    stmt.bind_column(
      first, SQL_C_CHAR, catalog.data, sizeof(catalog.data), &catalog.ind);
    stmt.bind_column(
      first + 1, SQL_C_CHAR, schema.data, sizeof(schema.data), &schema.ind);
    stmt.bind_column(
      first + 2, SQL_C_CHAR, table.data, sizeof(table.data), &table.ind);
  }

  bool asked_as(std::string_view catalog_name, std::string_view schema_name,
    std::string_view table_name) const {
    return catalog_name_matches(catalog_name, catalog.str()) &&
      catalog_name_matches(schema_name, schema.str()) &&
      catalog_name_matches(table_name, table.str());
  }

  table_row exact_row(bool schema_required) const {
    return { catalog.exact_str("catalog"),
      schema.exact_str("schema", !schema_required), table.exact_str("table") };
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

std::string exact_info_text(SQLHDBC connection, SQLUSMALLINT info) {
  char data[1024] = {};
  SQLSMALLINT length = 0;
  SQLRETURN status = SQLGetInfo(connection, info, data, sizeof(data), &length);
  if (status != SQL_SUCCESS) {
    throw backend::capability_not_supported(
      "exact catalog reads cannot obtain ODBC information " +
      std::to_string(info));
  }
  // An empty answer is a driver's way of saying the key does not apply to
  // it (SQL_SEARCH_PATTERN_ESCAPE, in particular); the specific caller
  // classifies that, not the general reader here.
  return exact_catalog_text({ data, sizeof(data) }, length,
    "ODBC information " + std::to_string(info), true);
}

std::string exact_namespace_escape(connection& conn,
  std::string_view catalog, std::string_view schema) {
  exact_catalog_arg_length(catalog);
  exact_catalog_arg_length(schema);
  if (catalog.empty()) {
    throw mapping_error("exact catalog reads require a nonempty catalog");
  }
  auto dbms = uniorm::detail::fold_lower(
    exact_info_text(conn.native(), SQL_DBMS_NAME));
  if (dbms == "postgresql") {
    if (schema.empty() ||
        catalog != exact_info_text(conn.native(), SQL_DATABASE_NAME)) {
      throw mapping_error(
        "exact PostgreSQL metadata requires the current catalog and a schema");
    }
  } else if (dbms == "mysql" || dbms == "mariadb") {
    if (!schema.empty()) {
      throw mapping_error("exact MySQL metadata requires an empty schema");
    }
  } else {
    throw backend::capability_not_supported(
      "exact catalog reads support only PostgreSQL, MySQL and MariaDB");
  }
  return exact_info_text(conn.native(), SQL_SEARCH_PATTERN_ESCAPE);
}

void require_pattern_metadata(odbc::statement& stmt) {
  SQLRETURN status = SQLSetStmtAttr(
    stmt.native(), SQL_ATTR_METADATA_ID, nullptr, SQL_IS_UINTEGER);
  SQLULEN metadata_id = SQL_TRUE;
  if (status != SQL_SUCCESS ||
      SQLGetStmtAttr(stmt.native(), SQL_ATTR_METADATA_ID, &metadata_id,
        sizeof(metadata_id), nullptr) != SQL_SUCCESS ||
      metadata_id != SQL_FALSE) {
    throw backend::capability_not_supported(
      "exact catalog reads require SQL_ATTR_METADATA_ID to remain false");
  }
}

void check_catalog_result(SQLRETURN status, odbc::statement& stmt,
  std::string const& context, bool exact) {
  odbc::throw_if_error(status, SQL_HANDLE_STMT, stmt.native(), context);
  if (exact && status != SQL_SUCCESS) {
    throw odbc_error("exact metadata warning: " + context,
      collect_diagnostics(SQL_HANDLE_STMT, stmt.native()));
  }
}

bool fetch_catalog_row(odbc::statement& stmt, bool exact) {
  if (!exact) {
    return stmt.fetch();
  }
  SQLRETURN status = SQLFetch(stmt.native());
  if (status == SQL_NO_DATA) {
    return false;
  }
  check_catalog_result(status, stmt, "fetch catalog row", true);
  return true;
}

std::vector<table_row> read_tables(connection& conn,
  std::string_view catalog, std::string_view schema, bool exact) {
  std::string escape;
  if (exact) {
    escape = exact_namespace_escape(conn, catalog, schema);
  }
  std::string schema_pattern = exact
    ? catalog_pattern_literal(schema, escape) : std::string(schema);
  std::string catalog_name = exact
    ? exact_tables_catalog_arg(catalog, escape) : std::string(catalog);
  std::string table_pattern = exact ? "%" : "";
  text_arg catalog_arg(catalog_name, exact);
  text_arg schema_arg(schema_pattern, exact);
  text_arg table_arg(table_pattern, exact);

  odbc::statement stmt(conn);
  if (exact) {
    require_pattern_metadata(stmt);
  }
  SQLRETURN status = SQLTables(stmt.native(), catalog_arg.ptr,
    catalog_arg.len, schema_arg.ptr, schema_arg.len, table_arg.ptr,
    table_arg.len, nullptr, 0);
  check_catalog_result(status, stmt, "SQLTables", exact);

  reported_names names;
  char_buffer table_type;
  names.bind(stmt, 1);
  stmt.bind_column(
    4, SQL_C_CHAR, table_type.data, sizeof(table_type.data),
    &table_type.ind);

  std::vector<table_row> rows;
  while (fetch_catalog_row(stmt, exact)) {
    std::string type = exact
      ? table_type.exact_str("table type") : table_type.str();
    if (type != "TABLE" && type != "BASE TABLE") {
      continue;
    }
    if (exact) {
      auto row = names.exact_row(!schema.empty());
      if (exact_catalog_namespace_matches(catalog, schema, row)) {
        rows.push_back(std::move(row));
      }
    } else if (names.asked_as(catalog, schema, {})) {
      rows.push_back(table_row{
        names.catalog.str(), names.schema.str(), names.table.str() });
    }
  }
  return rows;
}

std::vector<column_row> read_table_columns(connection& conn,
  meta::table_ref const& table, bool exact) {
  std::string escape;
  if (exact) {
    exact_catalog_arg_length(table.name);
    if (table.name.empty()) {
      throw mapping_error("exact column metadata requires a nonempty table");
    }
    escape = exact_namespace_escape(conn, table.catalog, table.schema);
  }
  // Unlike SQLTables, SQLColumns takes CatalogName as a literal value.
  std::string catalog(table.catalog);
  std::string schema = exact
    ? catalog_pattern_literal(table.schema, escape) : table.schema;
  std::string table_name = exact
    ? catalog_pattern_literal(table.name, escape) : table.name;
  std::string column_pattern = exact ? "%" : "";
  text_arg catalog_arg(catalog, exact);
  text_arg schema_arg(schema, exact);
  text_arg table_arg(table_name, exact);
  text_arg column_arg(column_pattern, exact);

  odbc::statement stmt(conn);
  if (exact) {
    require_pattern_metadata(stmt);
  }
  SQLRETURN status = SQLColumns(stmt.native(), catalog_arg.ptr,
    catalog_arg.len, schema_arg.ptr, schema_arg.len, table_arg.ptr,
    table_arg.len, column_arg.ptr, column_arg.len);
  check_catalog_result(status, stmt, "SQLColumns(" + table.name + ")", exact);

  reported_names names;
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

  names.bind(stmt, 1);
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
  while (fetch_catalog_row(stmt, exact)) {
    if (exact) {
      auto identity = names.exact_row(!table.schema.empty());
      if (!exact_catalog_table_matches(table, identity)) {
        continue;
      }
    } else if (name.is_null() ||
               !names.asked_as(table.catalog, table.schema, table.name)) {
      continue;
    }
    column_row row;
    row.shape.name = exact ? name.exact_str("column") : name.str();
    row.shape.type = data_type_ind == SQL_NULL_DATA
                       ? sql_type::other
                       : sql_type_from_native(data_type);
    row.type_name = exact
      ? type_name.exact_str("type name", true) : type_name.str();
    row.size = size_ind == SQL_NULL_DATA ? 0 : column_size;
    row.decimals = decimals_ind == SQL_NULL_DATA ? 0 : decimals;
    row.shape.nullable =
      nullable_ind != SQL_NULL_DATA && nullable != SQL_NO_NULLS;
    if (default_value.ind != SQL_NULL_DATA) {
      row.default_value = exact
        ? default_value.exact_str("column default", true) : default_value.str();
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace

std::string odbc_schema_meta::database_name() {
  return info_text(conn_.native(), SQL_DATABASE_NAME);
}

std::vector<meta::table_row> odbc_schema_meta::tables(
  std::string_view catalog, std::string_view schema) {
  return read_tables(conn_, catalog, schema, false);
}

std::vector<meta::column_row> odbc_schema_meta::table_columns(
  table_ref const& table) {
  return read_table_columns(conn_, table, false);
}

std::vector<meta::table_row> odbc_schema_meta::exact_tables(
  std::string_view catalog, std::string_view schema) {
  return read_tables(conn_, catalog, schema, true);
}

std::vector<meta::column_row> odbc_schema_meta::exact_table_columns(
  table_ref const& table) {
  return read_table_columns(conn_, table, true);
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

  reported_names names;
  char_buffer name;
  names.bind(stmt, 1);
  stmt.bind_column(4, SQL_C_CHAR, name.data, sizeof(name.data), &name.ind);

  std::vector<std::string> columns;
  while (stmt.fetch()) {
    if (!name.is_null() && names.asked_as(cat, sch, tbl)) {
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
  reported_names names;
  stmt.bind_column(
    3, SQL_C_CHAR, pk_table.data, sizeof(pk_table.data), &pk_table.ind);
  stmt.bind_column(
    4, SQL_C_CHAR, pk_column.data, sizeof(pk_column.data), &pk_column.ind);
  names.bind(stmt, 5);
  stmt.bind_column(
    8, SQL_C_CHAR, fk_column.data, sizeof(fk_column.data), &fk_column.ind);

  std::vector<foreign_key_row> rows;
  while (stmt.fetch()) {
    if (!names.asked_as(cat, sch, tbl)) {
      continue;
    }
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

  reported_names names;
  SQLSMALLINT non_unique = 0;
  SQLLEN non_unique_ind = SQL_NULL_DATA;
  char_buffer index_name;
  SQLSMALLINT row_type = 0;
  SQLLEN row_type_ind = SQL_NULL_DATA;
  char_buffer column;

  names.bind(stmt, 1);
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
    if (!names.asked_as(cat, sch, tbl)) {
      continue;
    }
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
