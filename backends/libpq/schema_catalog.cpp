#include "schema_catalog.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <libpq-fe.h>

#include "error.hpp"
#include "handles.hpp"

namespace uniorm::libpq {

namespace {

using meta = schema_meta;
using table_row = meta::table_row;
using column_row = meta::column_row;

// The schemas a mapping can see: not the system catalogs, toast, or temp. Every
// read shares this so an empty schema means the same visible set throughout.
char const* const k_visible_schema =
  "AND n.nspname NOT IN ('pg_catalog','information_schema') "
  "AND n.nspname NOT LIKE 'pg_toast%' AND n.nspname NOT LIKE 'pg_temp%' ";

std::string cell(PGresult const* result, int row, int column) {
  if (PQgetisnull(result, row, column)) {
    return {};
  }
  return std::string(PQgetvalue(result, row, column),
    static_cast<std::size_t>(PQgetlength(result, row, column)));
}

bool cell_is_null(PGresult const* result, int row, int column) {
  return PQgetisnull(result, row, column) != 0;
}

// One catalog read. Names go as $n text parameters, never folded into the SQL,
// and the read joins the session's open transaction so it sees the same
// uncommitted work the caller is inside.
detail::result_holder run_catalog(connection& conn, std::string const& sql,
  std::vector<std::string> const& args, char const* context) {
  conn.open_transaction();
  std::vector<char const*> values;
  values.reserve(args.size());
  for (auto const& arg : args) {
    values.push_back(arg.c_str());
  }
  detail::result_holder answer{ PQexecParams(conn.native(), sql.c_str(),
    static_cast<int>(values.size()), nullptr,
    values.empty() ? nullptr : values.data(), nullptr, nullptr, 0) };
  throw_if_failed(answer.get(), context, { PGRES_TUPLES_OK });
  return answer;
}

std::pair<std::string, std::string> default_namespace(connection& conn) {
  auto answer = run_catalog(conn, "SELECT current_database(), current_schema()",
    {}, "catalog namespace");
  PGresult const* result = answer.get();
  return { cell(result, 0, 0), cell(result, 0, 1) };
}

// Exact mode pins an empty catalog/schema to the session's current names so the
// byte filter afterwards has something concrete to compare against. A non-exact
// read leaves an empty schema open to every visible schema instead.
void resolve_exact_namespace(
  connection& conn, std::string& catalog, std::string& schema) {
  if (!catalog.empty() && !schema.empty()) {
    return;
  }
  auto defaults = default_namespace(conn);
  if (catalog.empty()) {
    catalog = defaults.first;
  }
  if (schema.empty()) {
    schema = defaults.second;
  }
}

std::vector<table_row> read_tables(connection& conn, std::string_view catalog,
  std::string_view schema, bool exact) {
  std::string catalog_arg(catalog);
  std::string schema_arg(schema);
  if (exact) {
    resolve_exact_namespace(conn, catalog_arg, schema_arg);
  }
  auto answer = run_catalog(conn,
    std::string("SELECT current_database(), n.nspname, c.relname "
                "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
                "WHERE c.relkind IN ('r','p') "
                "AND ($1 = '' OR n.nspname = $1) ") +
      k_visible_schema +
      "AND ($2 = '' OR current_database() = $2) "
      "ORDER BY n.nspname, c.relname",
    { schema_arg, catalog_arg }, "catalog tables");

  PGresult const* result = answer.get();
  int const rows = PQntuples(result);
  std::vector<table_row> out;
  for (int row = 0; row < rows; ++row) {
    table_row entry{ cell(result, row, 0), cell(result, row, 1),
      cell(result, row, 2) };
    if (exact && (entry.catalog != catalog_arg || entry.schema != schema_arg)) {
      continue;
    }
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<column_row> read_columns(
  connection& conn, meta::table_ref const& table, bool exact) {
  std::string catalog_arg(table.catalog);
  std::string schema_arg(table.schema);
  if (exact) {
    resolve_exact_namespace(conn, catalog_arg, schema_arg);
  }
  auto answer = run_catalog(conn,
    std::string(
      "SELECT current_database(), n.nspname, c.relname, a.attname, "
      "a.atttypid, format_type(a.atttypid, a.atttypmod), a.atttypmod, "
      "a.attnotnull, pg_get_expr(d.adbin, d.adrelid) "
      "FROM pg_attribute a "
      "JOIN pg_class c ON c.oid = a.attrelid "
      "JOIN pg_namespace n ON n.oid = c.relnamespace "
      "LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
      "WHERE c.relname = $1 "
      "AND ($2 = '' OR n.nspname = $2) ") +
      k_visible_schema +
      "AND ($3 = '' OR current_database() = $3) "
      "AND a.attnum > 0 AND NOT a.attisdropped "
      "ORDER BY a.attnum",
    { table.name, schema_arg, catalog_arg }, "catalog columns");

  PGresult const* result = answer.get();
  int const rows = PQntuples(result);
  std::vector<column_row> out;
  for (int row = 0; row < rows; ++row) {
    if (exact && (cell(result, row, 0) != catalog_arg ||
                   cell(result, row, 1) != schema_arg ||
                   cell(result, row, 2) != table.name)) {
      continue;
    }
    column_row entry;
    entry.shape.name = cell(result, row, 3);
    unsigned const native =
      static_cast<unsigned>(std::stoul(cell(result, row, 4)));
    entry.shape.type = sql_type_from_native(native);
    entry.type_name = cell(result, row, 5);
    auto metrics =
      catalog_column_metrics(native, std::stoi(cell(result, row, 6)));
    entry.size = metrics.size;
    entry.decimals = metrics.decimals;
    entry.shape.nullable = cell(result, row, 7) != "t";
    if (!cell_is_null(result, row, 8)) {
      entry.default_value = cell(result, row, 8);
    }
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<std::string> read_primary_key(
  connection& conn, meta::table_ref const& table) {
  auto answer = run_catalog(conn,
    std::string(
      "SELECT a.attname "
      "FROM pg_constraint con "
      "JOIN pg_class c ON c.oid = con.conrelid "
      "JOIN pg_namespace n ON n.oid = c.relnamespace "
      "JOIN unnest(con.conkey) WITH ORDINALITY AS k(attnum, ord) ON true "
      "JOIN pg_attribute a ON a.attrelid = con.conrelid AND a.attnum = "
      "k.attnum "
      "WHERE con.contype = 'p' AND c.relname = $1 "
      "AND ($2 = '' OR n.nspname = $2) ") +
      k_visible_schema +
      "AND ($3 = '' OR current_database() = $3) "
      "ORDER BY k.ord",
    { table.name, table.schema, table.catalog }, "catalog primary key");

  PGresult const* result = answer.get();
  int const rows = PQntuples(result);
  std::vector<std::string> out;
  for (int row = 0; row < rows; ++row) {
    out.push_back(cell(result, row, 0));
  }
  return out;
}

std::vector<meta::foreign_key_row> read_foreign_keys(
  connection& conn, meta::table_ref const& table) {
  auto answer = run_catalog(conn,
    std::string(
      "SELECT rc.relname, ra.attname, la.attname "
      "FROM pg_constraint con "
      "JOIN pg_class c ON c.oid = con.conrelid "
      "JOIN pg_namespace n ON n.oid = c.relnamespace "
      "JOIN pg_class rc ON rc.oid = con.confrelid "
      "JOIN unnest(con.conkey, con.confkey) WITH ORDINALITY "
      "AS k(fknum, pknum, ord) ON true "
      "JOIN pg_attribute la ON la.attrelid = con.conrelid AND la.attnum = "
      "k.fknum "
      "JOIN pg_attribute ra ON ra.attrelid = con.confrelid AND ra.attnum = "
      "k.pknum "
      "WHERE con.contype = 'f' AND c.relname = $1 "
      "AND ($2 = '' OR n.nspname = $2) ") +
      k_visible_schema +
      "AND ($3 = '' OR current_database() = $3) "
      "ORDER BY k.ord",
    { table.name, table.schema, table.catalog }, "catalog foreign keys");

  PGresult const* result = answer.get();
  int const rows = PQntuples(result);
  std::vector<meta::foreign_key_row> out;
  for (int row = 0; row < rows; ++row) {
    out.push_back(meta::foreign_key_row{
      cell(result, row, 0), cell(result, row, 1), cell(result, row, 2) });
  }
  return out;
}

std::vector<meta::index_row> read_indexes(
  connection& conn, meta::table_ref const& table) {
  auto answer = run_catalog(conn,
    std::string(
      "SELECT ic.relname, i.indisunique, a.attname "
      "FROM pg_index i "
      "JOIN pg_class c ON c.oid = i.indrelid "
      "JOIN pg_namespace n ON n.oid = c.relnamespace "
      "JOIN pg_class ic ON ic.oid = i.indexrelid "
      "JOIN unnest(i.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
      "LEFT JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = "
      "k.attnum "
      "WHERE c.relname = $1 "
      "AND ($2 = '' OR n.nspname = $2) ") +
      k_visible_schema +
      "AND ($3 = '' OR current_database() = $3) "
      "ORDER BY ic.relname, k.ord",
    { table.name, table.schema, table.catalog }, "catalog indexes");

  PGresult const* result = answer.get();
  int const rows = PQntuples(result);
  std::vector<meta::index_row> out;
  for (int row = 0; row < rows; ++row) {
    std::string name = cell(result, row, 0);
    if (name.empty()) {
      continue;
    }
    auto it = std::find_if(out.begin(), out.end(),
      [&](meta::index_row const& entry) { return entry.name == name; });
    if (it == out.end()) {
      out.push_back(meta::index_row{ name, {}, false });
      it = std::prev(out.end());
    }
    it->unique = cell(result, row, 1) == "t";
    // An expression key has no column behind it (attnum 0); the plain keys
    // are the ones a mapping names.
    if (!cell_is_null(result, row, 2)) {
      it->columns.push_back(cell(result, row, 2));
    }
  }
  return out;
}

}  // namespace

std::string pq_schema_meta::database_name() {
  char const* name = PQdb(conn_.native());
  return name == nullptr ? std::string() : std::string(name);
}

std::vector<meta::table_row> pq_schema_meta::tables(
  std::string_view catalog, std::string_view schema) {
  return read_tables(conn_, catalog, schema, false);
}

std::vector<meta::column_row> pq_schema_meta::table_columns(
  table_ref const& table) {
  return read_columns(conn_, table, false);
}

std::vector<meta::table_row> pq_schema_meta::exact_tables(
  std::string_view catalog, std::string_view schema) {
  return read_tables(conn_, catalog, schema, true);
}

std::vector<meta::column_row> pq_schema_meta::exact_table_columns(
  table_ref const& table) {
  return read_columns(conn_, table, true);
}

std::vector<std::string> pq_schema_meta::primary_key(table_ref const& table) {
  return read_primary_key(conn_, table);
}

std::vector<meta::foreign_key_row> pq_schema_meta::foreign_keys(
  table_ref const& table) {
  return read_foreign_keys(conn_, table);
}

std::vector<meta::index_row> pq_schema_meta::indexes(table_ref const& table) {
  return read_indexes(conn_, table);
}

}  // namespace uniorm::libpq