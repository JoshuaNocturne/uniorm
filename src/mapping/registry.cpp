#include "uniorm/mapping/registry.hpp"

#include <unordered_map>
#include <utility>

#include "uniorm/connection.hpp"
#include "uniorm/odbc/error.hpp"
#include "uniorm/odbc/statement.hpp"

namespace uniorm {

std::string const& entity_meta::column_name(member_key const& key) const {
  for (auto const& c : columns) {
    if (c.key == key) {
      return c.column;
    }
  }
  throw mapping_error("member is not mapped to a column");
}

void entity_meta::populate(void* obj, row const& r) const {
  for (auto const& c : columns) {
    c.write(obj, r.at(c.column));
  }
}

namespace {

struct schema_column {
  SQLINTEGER data_type = 0;
  bool nullable = false;
};

std::unordered_map<std::string, schema_column> load_table_schema(
  odbc::connection& dbc, std::string const& table) {
  odbc::statement stmt(dbc);
  SQLRETURN rc = SQLColumns(stmt.native(), nullptr, 0, nullptr, 0,
    reinterpret_cast<SQLCHAR*>(const_cast<char*>(table.c_str())), SQL_NTS,
    nullptr, 0);
  odbc::throw_if_error(
    rc, SQL_HANDLE_STMT, stmt.native(), "SQLColumns(" + table + ")");

  struct buffers {
    char name[256] = {};
    SQLLEN name_ind = SQL_NULL_DATA;
    SQLINTEGER data_type = 0;
    SQLLEN type_ind = SQL_NULL_DATA;
    SQLSMALLINT nullable = SQL_NO_NULLS;
    SQLLEN null_ind = SQL_NULL_DATA;
  } buf;

  stmt.bind_column(4, SQL_C_CHAR, buf.name, sizeof(buf.name), &buf.name_ind);
  stmt.bind_column(
    5, SQL_C_SLONG, &buf.data_type, sizeof(buf.data_type), &buf.type_ind);
  stmt.bind_column(
    11, SQL_C_SSHORT, &buf.nullable, sizeof(buf.nullable), &buf.null_ind);

  std::unordered_map<std::string, schema_column> schema;
  while (stmt.fetch()) {
    if (buf.name_ind == SQL_NULL_DATA) {
      continue;
    }
    std::string name(buf.name, static_cast<std::size_t>(buf.name_ind));
    schema_column col;
    col.data_type = buf.data_type;
    col.nullable = buf.nullable != SQL_NO_NULLS;
    schema.emplace(std::move(name), col);
  }
  return schema;
}

}  // namespace

void orm::validate(connection& conn, validation_mode mode) {
  auto& dbc = conn.odbc_conn();
  for (auto const& [type, meta] : entities_) {
    auto schema = load_table_schema(dbc, meta.table);
    if (schema.empty()) {
      throw mapping_error("table not found: " + meta.table);
    }
    for (auto const& c : meta.columns) {
      auto it = schema.find(c.column);
      if (it == schema.end()) {
        throw mapping_error(
          "column not found in table " + meta.table + ": " + c.column);
      }
      if (mode == validation_mode::strict && it->second.nullable &&
          !c.nullable) {
        throw mapping_error("column " + meta.table + "." + c.column +
                            " is nullable but the mapped member is not "
                            "std::optional");
      }
    }
  }
}

}  // namespace uniorm
