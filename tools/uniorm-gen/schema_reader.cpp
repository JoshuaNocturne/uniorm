#include "schema_reader.hpp"

#include <algorithm>
#include <cctype>
#include <map>

#include <sql.h>
#include <sqlext.h>

#include "uniorm/error.hpp"
#include "uniorm/odbc/connection.hpp"
#include "uniorm/odbc/error.hpp"
#include "uniorm/odbc/statement.hpp"

namespace uniorm::gen {

namespace {

struct text_arg {
  SQLCHAR* ptr = nullptr;
  SQLSMALLINT len = 0;

  text_arg() = default;
  explicit text_arg(std::optional<std::string> const& s) {
    if (s) {
      ptr = reinterpret_cast<SQLCHAR*>(const_cast<char*>(s->c_str()));
      len = SQL_NTS;
    }
  }
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

  std::string str() const {
    if (ind <= 0 || ind == SQL_NULL_DATA) {
      return {};
    }
    return std::string(
      data, static_cast<std::size_t>(std::min<SQLLEN>(ind, sizeof(data) - 1)));
  }
};

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::vector<table_model> list_tables(
  odbc::connection& dbc, read_options const& opts) {
  odbc::statement stmt(dbc);
  text_arg cat(opts.catalog);
  text_arg sch(opts.schema);
  SQLRETURN rc = SQLTables(
    stmt.native(), cat.ptr, cat.len, sch.ptr, sch.len, nullptr, 0, nullptr, 0);
  odbc::throw_if_error(rc, SQL_HANDLE_STMT, stmt.native(), "SQLTables");

  char_buffer catalog;
  char_buffer schema;
  char_buffer name;
  char_buffer type;
  stmt.bind_column(
    1, SQL_C_CHAR, catalog.data, sizeof(catalog.data), &catalog.ind);
  stmt.bind_column(
    2, SQL_C_CHAR, schema.data, sizeof(schema.data), &schema.ind);
  stmt.bind_column(3, SQL_C_CHAR, name.data, sizeof(name.data), &name.ind);
  stmt.bind_column(4, SQL_C_CHAR, type.data, sizeof(type.data), &type.ind);

  std::vector<table_model> all;
  while (stmt.fetch()) {
    std::string t = type.str();
    if (t == "TABLE" || t == "BASE TABLE") {
      table_model m;
      m.catalog = catalog.str();
      m.schema = schema.str();
      m.name = name.str();
      all.push_back(std::move(m));
    }
  }

  if (opts.tables.empty()) {
    return all;
  }
  std::vector<table_model> selected;
  for (std::string const& want : opts.tables) {
    table_model const* hit = nullptr;
    for (table_model const& have : all) {
      if (have.name == want) {
        hit = &have;
        break;
      }
    }
    if (hit == nullptr) {
      // Some servers (lower_case_table_names) report table names with
      // different case than requested.
      for (table_model const& have : all) {
        if (to_lower(have.name) == to_lower(want)) {
          hit = &have;
          break;
        }
      }
    }
    if (hit == nullptr) {
      throw uniorm_error("table not found: " + want);
    }
    selected.push_back(*hit);
  }
  return selected;
}

void read_columns(odbc::connection& dbc, table_model& table) {
  odbc::statement stmt(dbc);
  text_arg cat(table.catalog);
  text_arg sch(table.schema);
  text_arg tbl(table.name);
  SQLRETURN rc = SQLColumns(stmt.native(), cat.ptr, cat.len, sch.ptr, sch.len,
    tbl.ptr, tbl.len, nullptr, 0);
  odbc::throw_if_error(
    rc, SQL_HANDLE_STMT, stmt.native(), "SQLColumns(" + table.name + ")");

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
  stmt.bind_column(9, SQL_C_SSHORT, &decimals, sizeof(decimals), &decimals_ind);
  stmt.bind_column(
    11, SQL_C_SSHORT, &nullable, sizeof(nullable), &nullable_ind);
  stmt.bind_column(13, SQL_C_CHAR, default_value.data,
    sizeof(default_value.data), &default_value.ind);

  while (stmt.fetch()) {
    column_model col;
    col.name = name.str();
    col.data_type = data_type_ind == SQL_NULL_DATA ? 0 : data_type;
    col.type_name = type_name.str();
    col.size = size_ind == SQL_NULL_DATA ? 0 : column_size;
    col.decimals = decimals_ind == SQL_NULL_DATA ? 0 : decimals;
    col.nullable = nullable_ind != SQL_NULL_DATA && nullable != SQL_NO_NULLS;
    if (default_value.ind != SQL_NULL_DATA) {
      col.default_value = default_value.str();
    }
    table.columns.push_back(std::move(col));
  }
}

void read_primary_keys(odbc::connection& dbc, table_model& table) {
  odbc::statement stmt(dbc);
  text_arg cat(table.catalog);
  text_arg sch(table.schema);
  text_arg tbl(table.name);
  SQLRETURN rc = SQLPrimaryKeys(
    stmt.native(), cat.ptr, cat.len, sch.ptr, sch.len, tbl.ptr, tbl.len);
  odbc::throw_if_error(
    rc, SQL_HANDLE_STMT, stmt.native(), "SQLPrimaryKeys(" + table.name + ")");

  char_buffer name;
  stmt.bind_column(4, SQL_C_CHAR, name.data, sizeof(name.data), &name.ind);
  while (stmt.fetch()) {
    std::string pk = name.str();
    for (column_model& c : table.columns) {
      if (c.name == pk) {
        c.primary_key = true;
      }
    }
  }
}

void read_foreign_keys(odbc::connection& dbc, table_model& table) {
  odbc::statement stmt(dbc);
  text_arg cat(table.catalog);
  text_arg sch(table.schema);
  text_arg tbl(table.name);
  SQLRETURN rc = SQLForeignKeys(stmt.native(), nullptr, 0, nullptr, 0, nullptr,
    0, cat.ptr, cat.len, sch.ptr, sch.len, tbl.ptr, tbl.len);
  odbc::throw_if_error(
    rc, SQL_HANDLE_STMT, stmt.native(), "SQLForeignKeys(" + table.name + ")");

  char_buffer pk_table;
  char_buffer pk_column;
  char_buffer fk_column;
  stmt.bind_column(
    3, SQL_C_CHAR, pk_table.data, sizeof(pk_table.data), &pk_table.ind);
  stmt.bind_column(
    4, SQL_C_CHAR, pk_column.data, sizeof(pk_column.data), &pk_column.ind);
  stmt.bind_column(
    8, SQL_C_CHAR, fk_column.data, sizeof(fk_column.data), &fk_column.ind);

  // Ordered KEY_SEQ within each referenced table; keep that order.
  std::vector<fk_model> fks;
  while (stmt.fetch()) {
    std::string ref = pk_table.str();
    auto it = std::find_if(fks.begin(), fks.end(),
      [&](fk_model const& m) { return m.pk_table == ref; });
    if (it == fks.end()) {
      fks.push_back(fk_model{ ref, {} });
      it = std::prev(fks.end());
    }
    it->columns.emplace_back(fk_column.str(), pk_column.str());
  }
  table.foreign_keys = std::move(fks);
}

void read_indexes(odbc::connection& dbc, table_model& table) {
  odbc::statement stmt(dbc);
  text_arg cat(table.catalog);
  text_arg sch(table.schema);
  text_arg tbl(table.name);
  SQLRETURN rc = SQLStatistics(stmt.native(), cat.ptr, cat.len, sch.ptr,
    sch.len, tbl.ptr, tbl.len, SQL_INDEX_ALL, SQL_QUICK);
  odbc::throw_if_error(
    rc, SQL_HANDLE_STMT, stmt.native(), "SQLStatistics(" + table.name + ")");

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
  stmt.bind_column(7, SQL_C_SSHORT, &row_type, sizeof(row_type), &row_type_ind);
  stmt.bind_column(
    9, SQL_C_CHAR, column.data, sizeof(column.data), &column.ind);

  std::map<std::string, index_model> by_name;
  while (stmt.fetch()) {
    // TYPE 0 is the table-statistics pseudo row, not an index.
    if (row_type_ind == SQL_NULL_DATA || row_type != SQL_INDEX_OTHER) {
      continue;
    }
    std::string iname = index_name.str();
    if (iname.empty()) {
      continue;
    }
    index_model& idx = by_name[iname];
    idx.name = iname;
    idx.unique = non_unique_ind != SQL_NULL_DATA && non_unique == 0;
    idx.columns.push_back(column.str());
  }
  for (auto& [_, idx] : by_name) {
    table.indexes.push_back(std::move(idx));
  }
}

}  // namespace

schema_model read_schema(odbc::connection& dbc, read_options const& opts,
  std::vector<std::string>* warnings) {
  schema_model model;
  for (table_model& table : list_tables(dbc, opts)) {
    read_columns(dbc, table);
    read_primary_keys(dbc, table);
    read_foreign_keys(dbc, table);
    read_indexes(dbc, table);
    if (table.columns.empty() && warnings != nullptr) {
      warnings->push_back("table has no columns: " + table.name);
    }
    model.tables.push_back(std::move(table));
  }
  return model;
}

std::string database_name(odbc::connection& dbc) {
  char buf[256] = {};
  SQLSMALLINT len = 0;
  SQLRETURN rc =
    SQLGetInfo(dbc.native(), SQL_DATABASE_NAME, buf, sizeof(buf), &len);
  if (!SQL_SUCCEEDED(rc) || len <= 0) {
    return {};
  }
  return std::string(buf, static_cast<std::size_t>(len));
}

}  // namespace uniorm::gen
