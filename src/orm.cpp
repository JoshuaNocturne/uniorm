#include "uniorm/orm.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

#include "orm_mapping.hpp"
#include "uniorm/backend/backend.hpp"
#include "uniorm/builder/builder.hpp"
#include "uniorm/dialect.hpp"
#include "uniorm/result_set.hpp"
#include "uniorm/transaction.hpp"
#include <uniorm/connection.hpp>

namespace uniorm {

namespace {

// Begin a lease in the mode its new owner asked for, discarding any pending
// work a previous lease left behind: enabling autocommit would commit it.
void adopt_connection(connection& conn, bool autocommit,
  std::optional<dialect::identifier_case> identifiers) {
  if (!conn.autocommit()) {
    conn.rollback();
  }
  conn.set_autocommit(autocommit);
  conn.identifier_case(identifiers);
}

}  // namespace

// --- Connection lifecycle ---

orm::orm(std::string_view connection_string)
  : pooled_conn_(connection_pool_registry::instance().acquire(
      std::string(connection_string))) {
  adopt_connection(native_connection(), auto_commit_, identifiers_);
}

orm::orm(connection_spec const& spec)
  : pooled_conn_(connection_pool_registry::instance().acquire(spec)) {
  adopt_connection(native_connection(), auto_commit_, identifiers_);
}

orm::orm(connection_pool& pool) : pooled_conn_(pool.acquire()) {
  adopt_connection(native_connection(), auto_commit_, identifiers_);
}

orm::~orm() = default;

orm::orm(orm&&) noexcept = default;

orm& orm::operator=(orm&&) noexcept = default;

void orm::connect(std::string_view connection_string) {
  clear_identifier_resolution();
  pooled_conn_ = connection_pool_registry::instance().acquire(
    std::string(connection_string));
  adopt_connection(native_connection(), auto_commit_, identifiers_);
}

void orm::connect(connection_spec const& spec) {
  clear_identifier_resolution();
  pooled_conn_ = connection_pool_registry::instance().acquire(spec);
  adopt_connection(native_connection(), auto_commit_, identifiers_);
}

void orm::disconnect() {
  clear_identifier_resolution();
  ensure_connected();
  pooled_conn_.reset();  // returns connection to pool
}

void orm::ensure_connected() const {
  if (!pooled_conn_ || !pooled_conn_->get().is_open()) {
    throw uniorm_error("orm: not connected; call connect() first");
  }
}

connection& orm::native_connection() {
  ensure_connected();
  return pooled_conn_->get();
}

schema_meta& orm::schema() {
  return native_connection().schema();
}

// --- Entity mapping registry ---

entity_meta const* orm::find(std::type_index type) const {
  auto it = entities_.find(type);
  return it == entities_.end() ? nullptr : &it->second;
}

std::size_t orm::size() const noexcept {
  return entities_.size();
}

void orm::resolve_identifiers(
  std::string_view catalog_name, std::string_view schema_name) {
  auto& connection = native_connection();
  auto const qualification = connection.sql_dialect().table_qualification;
  if (qualification == dialect::qualification::unsupported) {
    throw backend::capability_not_supported(
      "identifier resolution is unsupported for " + connection.dbms_name());
  }
  if (catalog_name.empty() ||
      catalog_name.find('\0') != std::string_view::npos ||
      schema_name.find('\0') != std::string_view::npos) {
    throw mapping_error("identifier resolution requires an explicit catalog "
                        "and names without NUL bytes");
  }
  auto& metadata = connection.schema();
  if (qualification == dialect::qualification::schema) {
    if (schema_name.empty() || catalog_name != metadata.database_name()) {
      throw mapping_error("PostgreSQL identifier resolution requires the "
                          "current database and an explicit schema");
    }
  } else if (!schema_name.empty()) {
    throw mapping_error("MySQL identifier resolution requires a database "
                        "and an empty schema");
  }
  detail::identifier_catalog catalog(metadata, catalog_name, schema_name);
  using pending_resolution =
    std::pair<entity_meta*, std::optional<identifier_resolution>>;
  std::vector<pending_resolution> pending;
  pending.reserve(entities_.size());
  for (auto& [type, entity] : entities_) {
    pending.emplace_back(&entity, catalog.resolve(entity));
  }
  static_assert(
    std::is_nothrow_swappable_v<std::optional<identifier_resolution>>);
  for (auto& [entity, resolution] : pending) {
    entity->resolved.swap(resolution);
  }
}

void orm::clear_identifier_resolution() noexcept {
  for (auto& [type, entity] : entities_) {
    entity.resolved.reset();
  }
}

// --- Validation ---

void orm::validate(validation_mode mode) {
  auto& connection = native_connection();
  dialect const& dialect = connection.sql_dialect();
  detail::catalog catalog(connection.schema());
  for (auto const& [type, entity] : entities_) {
    detail::validate_entity(catalog, entity, mode, dialect);
  }
}

// --- Raw SQL operations ---

result_set orm::execute(std::string_view sql, params const& p) {
  ensure_connected();
  return pooled_conn_->get().execute(sql, p, row_array_size_);
}

std::size_t orm::execute_update(std::string_view sql, params const& p) {
  ensure_connected();
  return pooled_conn_->get().execute_update(sql, p);
}

// --- Dynamic (non-entity) operations ---

namespace {

bool blank(std::string_view s) {
  return std::all_of(
    s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

// Joins the accumulated WHERE clauses with " AND " and copies their
// parameters in the same order, so a caller who chained
// .where("a = ?", { 1 }).where("b = ?", { 2 }) gets the parameter positions
// they expect regardless of how many clauses there are. update_builder and
// remove_builder each keep a private where_clause; the shape -- { sql,
// bound } -- is the only thing this helper needs.
template <class Clause>
std::string render_where(
  std::vector<Clause> const& clauses, std::vector<sql_value>& bound) {
  std::string sql;
  for (std::size_t index = 0; index < clauses.size(); ++index) {
    if (index != 0) {
      sql += " AND ";
    }
    sql += clauses[index].sql;
    auto const& values = clauses[index].bound.values();
    bound.insert(bound.end(), values.begin(), values.end());
  }
  return sql;
}

}  // namespace

update_builder::update_builder(orm& db, std::string table)
  : orm_(&db), table_(std::move(table)) {}

update_builder& update_builder::where(std::string_view clause, params p) {
  if (blank(clause)) {
    throw uniorm_error("update: where() requires a non-blank clause");
  }
  wheres_.push_back({ std::string(clause), std::move(p) });
  return *this;
}

std::size_t update_builder::execute() {
  if (set_.empty()) {
    throw uniorm_error("update: no columns to set");
  }
  if (wheres_.empty()) {
    throw uniorm_error("update: refusing to execute without a WHERE clause");
  }
  dialect const& d = orm_->native_connection().sql_dialect();
  std::string sql = "UPDATE " + d.quote_identifier(table_) + " SET ";
  std::vector<sql_value> values;
  values.reserve(set_.size());
  for (std::size_t i = 0; i < set_.size(); ++i) {
    if (i != 0) {
      sql += ", ";
    }
    sql += d.quote_identifier(set_[i].first) + " = ?";
    values.push_back(set_[i].second);
  }
  sql += " WHERE " + render_where(wheres_, values);
  return orm_->execute_update(sql, params(std::move(values)));
}

remove_builder::remove_builder(orm& db, std::string table)
  : orm_(&db), table_(std::move(table)) {}

remove_builder& remove_builder::where(std::string_view clause, params p) {
  if (blank(clause)) {
    throw uniorm_error("remove: where() requires a non-blank clause");
  }
  wheres_.push_back({ std::string(clause), std::move(p) });
  return *this;
}

std::size_t remove_builder::execute() {
  if (wheres_.empty()) {
    throw uniorm_error("remove: refusing to execute without a WHERE clause");
  }
  dialect const& d = orm_->native_connection().sql_dialect();
  std::vector<sql_value> values;
  std::string where = render_where(wheres_, values);
  std::string sql =
    "DELETE FROM " + d.quote_identifier(table_) + " WHERE " + where;
  return orm_->execute_update(sql, params(std::move(values)));
}

update_builder orm::update(std::string_view table) {
  ensure_connected();
  return update_builder(*this, std::string(table));
}

remove_builder orm::remove(std::string_view table) {
  ensure_connected();
  return remove_builder(*this, std::string(table));
}

// --- Transaction ---

transaction orm::begin() {
  ensure_connected();
  return pooled_conn_->get().begin();
}

void orm::commit() {
  ensure_connected();
  pooled_conn_->get().commit();
}

void orm::rollback() {
  ensure_connected();
  pooled_conn_->get().rollback();
}

// --- Cache observability ---

unsigned long long orm::statement_cache_hits() const {
  ensure_connected();
  return pooled_conn_->get().statement_cache_hits();
}

unsigned long long orm::statement_cache_misses() const {
  ensure_connected();
  return pooled_conn_->get().statement_cache_misses();
}

std::size_t orm::statement_cache_size() const {
  ensure_connected();
  return pooled_conn_->get().statement_cache_size();
}

void orm::clear_statement_cache() {
  ensure_connected();
  pooled_conn_->get().clear_statement_cache();
}

// --- Configuration ---

std::size_t orm::row_array_size() const noexcept {
  return row_array_size_;
}

void orm::row_array_size(std::size_t size) noexcept {
  row_array_size_ = size > 0 ? size : default_row_array_size;
}

std::size_t orm::paramset_size() const noexcept {
  return paramset_size_;
}

void orm::paramset_size(std::size_t size) noexcept {
  paramset_size_ = size > 0 ? size : default_paramset_size;
}

orm::batch_policy orm::batch_write_policy() const noexcept {
  return batch_policy_;
}

void orm::batch_write_policy(batch_policy policy) noexcept {
  batch_policy_ = policy;
}

bool orm::auto_commit() const noexcept {
  return auto_commit_;
}

void orm::auto_commit(bool enabled) {
  // Set the connection first: a throw must not leave this orm reporting a
  // mode its connection is not in.
  if (pooled_conn_ && pooled_conn_->get().is_open()) {
    pooled_conn_->get().set_autocommit(enabled);
  }
  auto_commit_ = enabled;
}

std::optional<dialect::identifier_case> orm::identifier_case() const noexcept {
  return identifiers_;
}

void orm::identifier_case(std::optional<dialect::identifier_case> policy) {
  if (pooled_conn_ && pooled_conn_->get().is_open()) {
    pooled_conn_->get().identifier_case(policy);
  }
  identifiers_ = policy;
}

// --- Entity write pipeline ---
// Storage arrives type-erased as (data, element stride, row count); columns
// are addressed by index.

namespace {

std::string build_insert_sql(
  dialect const& dialect, entity_meta const& entity) {
  std::size_t const count = entity.columns.size();
  std::string sql = "INSERT INTO " + entity.table_sql(dialect) + " (";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      sql += ", ";
    }
    sql += entity.column_sql(index, dialect);
  }
  sql += ") VALUES (?";
  for (std::size_t index = 1; index < count; ++index) {
    sql += ", ?";
  }
  sql += ")";
  return sql;
}

std::size_t element_size_for(
  backend::buffer_type type, std::size_t max_var_size) {
  switch (type) {
  case backend::buffer_type::bit:
    return sizeof(unsigned char);
  case backend::buffer_type::int16:
    return sizeof(std::int16_t);
  case backend::buffer_type::int32:
    return sizeof(std::int32_t);
  case backend::buffer_type::int64:
    return sizeof(std::int64_t);
  case backend::buffer_type::float64:
    return sizeof(double);
  case backend::buffer_type::chars:
  case backend::buffer_type::bytes:
    return std::max(max_var_size + 1, std::size_t(65));
  case backend::buffer_type::timestamp_parts:
    return sizeof(backend::timestamp_parts);
  default:
    return 1;
  }
}

// Row `i` of a type-erased entity array. Vector storage is contiguous and
// sizeof is a multiple of alignof, so row addresses stay aligned.
void const* row_at(void const* rows, std::size_t row_stride, std::size_t i) {
  return static_cast<std::byte const*>(rows) + i * row_stride;
}

std::vector<std::size_t> all_columns(entity_meta const& m) {
  std::vector<std::size_t> out(m.columns.size());
  for (std::size_t ci = 0; ci < out.size(); ++ci) {
    out[ci] = ci;
  }
  return out;
}

std::vector<std::size_t> concat_columns(
  std::vector<std::size_t> const& head, std::vector<std::size_t> const& tail) {
  std::vector<std::size_t> out;
  out.reserve(head.size() + tail.size());
  out.insert(out.end(), head.begin(), head.end());
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

// `<col> = ?` for each index, joined by sep. Statement text and bound
// parameters are both spelled from one index list, so the two cannot drift.
std::string placeholder_group(entity_meta const& entity,
  std::vector<std::size_t> const& col_indices, dialect const& dialect,
  std::string_view separator) {
  std::string out;
  for (std::size_t index = 0; index < col_indices.size(); ++index) {
    if (index != 0) {
      out += separator;
    }
    out += entity.column_sql(col_indices[index], dialect) + " = ?";
  }
  return out;
}

std::vector<sql_value> extract_row(entity_meta const& m, void const* entity,
  std::vector<std::size_t> const& col_indices) {
  std::vector<sql_value> vals;
  vals.reserve(col_indices.size());
  for (auto ci : col_indices) {
    vals.push_back(m.columns[ci].read(entity));
  }
  return vals;
}

// One statement text per operation, so the rowwise and columnar channels of
// the same write also share a statement-cache key.
std::string update_statement(entity_meta const& entity, dialect const& dialect,
  std::vector<std::size_t> const& set_col_indices,
  std::vector<std::size_t> const& where_col_indices) {
  return "UPDATE " + entity.table_sql(dialect) + " SET " +
         placeholder_group(entity, set_col_indices, dialect, ", ") + " WHERE " +
         placeholder_group(entity, where_col_indices, dialect, " AND ");
}

std::string delete_statement(entity_meta const& entity, dialect const& dialect,
  std::vector<std::size_t> const& where_col_indices) {
  return "DELETE FROM " + entity.table_sql(dialect) + " WHERE " +
         placeholder_group(entity, where_col_indices, dialect, " AND ");
}

// --- Folded batch statements: a whole chunk of rows in one statement ---

// The name this dialect gives the batch's row source, and the name it gives
// the table being written. Both are spelled unquoted, so each server folds them
// into its own habit and the references below land on the same spelling.
std::string const batch_rows_alias = "uo_batch_rows";
std::string const batch_target_alias = "uo_batch_target";

// The chunk of rows as a query of its own: the table read for nothing gives
// the columns their names and their types, and each row after it is one
// parameter tuple. Types come from the table, so no parameter spells one out,
// and a value the entity leaves NULL keeps the column's own type.
std::string fold_row_source(entity_meta const& entity, dialect const& dialect,
  std::vector<std::size_t> const& columns, std::size_t row_count) {
  std::string out = "(SELECT ";
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (index != 0) {
      out += ", ";
    }
    out += entity.column_sql(columns[index], dialect);
  }
  out += " FROM " + entity.table_sql(dialect) + " WHERE 1 = 0";
  // Oracle reads a literal-only SELECT from a table, and `dual` is the one it
  // always has.
  bool const needs_from_dual =
    dialect.batch_write == dialect::batch_family::oracle;
  for (std::size_t row = 0; row < row_count; ++row) {
    out += " UNION ALL SELECT ";
    for (std::size_t index = 0; index < columns.size(); ++index) {
      if (index != 0) {
        out += ", ";
      }
      out += "?";
    }
    if (needs_from_dual) {
      out += " FROM dual";
    }
  }
  out += ")";
  return out;
}

// The table-value constructor: one VALUES list of parameter groups instead of
// a UNION ALL chain, with the column names the seed SELECT used to carry now
// named on the alias. The binds are still one group per row, so the parameter
// ceiling cuts this the same way; what shrinks is the text the parser walks.
// SQL Server takes AS before the alias, Oracle does not.
std::string values_row_source(entity_meta const& entity, dialect const& dialect,
  std::vector<std::size_t> const& columns, std::size_t row_count) {
  std::string out = "(VALUES ";
  for (std::size_t row = 0; row < row_count; ++row) {
    if (row != 0) {
      out += ", ";
    }
    out += "(";
    for (std::size_t index = 0; index < columns.size(); ++index) {
      if (index != 0) {
        out += ", ";
      }
      out += "?";
    }
    out += ")";
  }
  out += ") ";
  if (dialect.batch_write != dialect::batch_family::oracle) {
    out += "AS ";
  }
  out += batch_rows_alias + "(";
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (index != 0) {
      out += ", ";
    }
    out += entity.column_sql(columns[index], dialect);
  }
  out += ")";
  return out;
}

// `left.id = right.id AND left.name = right.name` for the columns named in
// `columns`, each spelled through the alias it is reached by.
std::string fold_match(entity_meta const& entity, dialect const& dialect,
  std::vector<std::size_t> const& columns, std::string_view left,
  std::string_view right, std::string_view connector) {
  std::string out;
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (index != 0) {
      out += connector;
    }
    std::string const column = entity.column_sql(columns[index], dialect);
    out += std::string(left) + "." + column + " = " + std::string(right) + "." +
           column;
  }
  return out;
}

// `<alias>.name = v.name, ...` -- the new values, read back off the row source.
// An empty `target` leaves the written-to column bare: a server that takes the
// SET target only from the table being updated reads it that way, and the
// column is never ambiguous there.
std::string fold_assignments(entity_meta const& entity, dialect const& dialect,
  std::vector<std::size_t> const& columns, std::string_view target) {
  std::string out;
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (index != 0) {
      out += ", ";
    }
    std::string const column = entity.column_sql(columns[index], dialect);
    if (!target.empty()) {
      out += std::string(target) + ".";
    }
    out += column + " = " + batch_rows_alias + "." + column;
  }
  return out;
}

// --- The array row source: one typed array per column of the chunk ---

// The type spellings a cast list can be built from. What a catalog calls a
// type is trusted for the shape of a name -- letters, digits, the spaces that
// separate the words of "timestamp with time zone", the bounds of
// "numeric(10,2)" -- and nothing else, so a name read off a server cannot
// carry statement syntax of its own. A server's word for an array type leads
// with an underscore, and a column of that type is not what one array deep
// holds: the row source would lay its elements out side by side instead of
// row by row.
bool array_cast_spelling(std::string const& spelling) {
  if (spelling.empty() || spelling.front() == '_') {
    return false;
  }
  for (char const symbol : spelling) {
    bool const plain =
      (symbol >= 'a' && symbol <= 'z') || (symbol >= 'A' && symbol <= 'Z') ||
      (symbol >= '0' && symbol <= '9') || symbol == ' ' || symbol == '.' ||
      symbol == '_' || symbol == '(' || symbol == ')' || symbol == ',';
    if (!plain) {
      return false;
    }
  }
  return true;
}

// A value's words inside the quotes an array element is written in. The words
// are the ones the server's own input function takes for the column's type,
// which is how the library's parameter bindings say the same thing: a bool as
// the digit an integer column also takes, a byte string in the server's hex
// form, a floating value with the digits that read back as the same number.
std::string array_element_text(
  sql_value const& value, std::string const& spelling) {
  return std::visit(
    [&spelling](auto const& held) -> std::string {
      using kind = std::decay_t<decltype(held)>;
      if constexpr (std::is_same_v<kind, std::monostate>) {
        return {};
      } else if constexpr (std::is_same_v<kind, bool>) {
        return held ? "1" : "0";
      } else if constexpr (std::is_same_v<kind, std::string>) {
        return held;
      } else if constexpr (std::is_same_v<kind, std::vector<std::byte>>) {
        static char const digits[] = "0123456789abcdef";
        std::string out = "\\x";
        for (std::byte const one : held) {
          auto const byte = static_cast<unsigned char>(one);
          out.push_back(digits[byte >> 4]);
          out.push_back(digits[byte & 0x0f]);
        }
        return out;
      } else if constexpr (std::is_same_v<kind, timestamp>) {
        std::string const folded = detail::fold_lower(spelling);
        bool const day_only = folded == "date";
        bool const clock_only = !day_only &&
                                folded.find("stamp") == std::string::npos &&
                                folded.find("time") != std::string::npos;
        return detail::text_of_timestamp(held, !clock_only, !day_only);
      } else if constexpr (std::is_same_v<kind, double>) {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof buffer, "%.17g", held);
        return buffer;
      } else {
        return std::to_string(held);
      }
    },
    value);
}

// One element of an array literal. Quoting every element is what keeps the
// array's own punctuation -- and the four letters of a word like NULL --
// inside the value, and only a value the entity leaves empty is written bare,
// since the bare word is the one spelling the server reads as no value. A
// NUL would cut the whole literal short, which is a different failure from
// the truncated string a bound parameter would give, so it is refused here.
std::string array_element(sql_value const& value, std::string const& spelling) {
  if (std::holds_alternative<std::monostate>(value)) {
    return "NULL";
  }
  std::string const text = array_element_text(value, spelling);
  if (text.find('\0') != std::string::npos) {
    throw uniorm_error("array element holds a NUL byte");
  }
  std::string quoted = "\"";
  for (char const symbol : text) {
    if (symbol == '"' || symbol == '\\') {
      quoted += '\\';
    }
    quoted += symbol;
  }
  return quoted + '"';
}

// The elements of one column as the literal a cast list takes. An empty chunk
// is the empty array, which the server reads as no rows at all.
std::string array_literal(std::vector<std::string> const& elements) {
  std::string out = "{";
  for (std::size_t index = 0; index < elements.size(); ++index) {
    if (index != 0) {
      out += ",";
    }
    out += elements[index];
  }
  return out + "}";
}

// The chunk as columns rather than rows: `unnest(?::bigint[], ?::text[]) AS
// uo_batch_rows(id, s)`. Two parameters however many rows the chunk holds,
// with no ceiling to cut them under, and one statement text for every chunk
// of the same columns.
std::string array_row_source(entity_meta const& entity, dialect const& dialect,
  std::vector<std::size_t> const& columns,
  std::vector<std::string> const& type_names) {
  std::string cast_list;
  std::string name_list;
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (index != 0) {
      cast_list += ", ";
      name_list += ", ";
    }
    cast_list += "?::" + type_names[columns[index]] + "[]";
    name_list += entity.column_sql(columns[index], dialect);
  }
  return "unnest(" + cast_list + ") AS " + batch_rows_alias + "(" + name_list +
         ")";
}

// Whether a folded sweep of these columns can be laid out as arrays. The
// family has to know the shape, the server has to have grown it, and every
// column the sweep touches has to have a type the cast list can be spelled
// from -- the values arrive as text, so nothing else gives them a type.
bool rows_as_arrays(dialect const& dialect,
  std::vector<std::string> const& type_names,
  std::vector<std::size_t> const& columns) {
  if (dialect.batch_write != dialect::batch_family::postgresql ||
      dialect.batch_rows != dialect::batch_source::typed_arrays) {
    return false;
  }
  for (std::size_t const column : columns) {
    if (column >= type_names.size() ||
        !array_cast_spelling(type_names[column])) {
      return false;
    }
  }
  return true;
}

// The fold the server is able to answer with, and the columns whose types it
// has to name. Reading the catalog is the last thing a sweep of a family that
// has no array shape needs to do, so it happens under that test.
struct fold_shape {
  bool arrays = false;
  std::vector<std::string> type_names;
};

fold_shape choose_fold_shape(connection& conn, entity_meta const& entity,
  dialect const& dialect, std::vector<std::size_t> const& columns) {
  fold_shape shape;
  if (dialect.batch_write != dialect::batch_family::postgresql ||
      dialect.batch_rows != dialect::batch_source::typed_arrays) {
    return shape;
  }
  // The names a column's type is filed under are the server's own, so a
  // mapping that spells them otherwise asks for nothing and gets the fold the
  // types do not have to name. The resolution pass is what reads them off the
  // catalog for those.
  schema_meta::table_ref table;
  std::vector<std::string> names;
  if (entity.resolved) {
    table = entity.resolved->table;
    names = entity.resolved->columns;
  } else {
    table.name = entity.table;
    names.reserve(entity.columns.size());
    for (auto const& column : entity.columns) {
      names.push_back(column.column);
    }
  }
  shape.type_names = conn.column_type_names(table, names);
  shape.arrays = rows_as_arrays(dialect, shape.type_names, columns);
  if (!shape.arrays) {
    // The seeded tuples need no type of their own, and a sweep that cannot
    // spell every column is better off not having asked.
    shape.type_names.clear();
  }
  return shape;
}

struct fold_plan {
  fold_shape shape;
  std::size_t chunk = 0;
};

// The row source a folded statement reaches its rows through, alias and all:
// arrays carry their column names with the source, value constructors name
// them on the alias, and seeded tuples are reached by the alias alone.
std::string fold_source(entity_meta const& entity, dialect const& dialect,
  std::vector<std::size_t> const& columns, std::size_t row_count,
  fold_shape const& shape) {
  if (shape.arrays) {
    return array_row_source(entity, dialect, columns, shape.type_names);
  }
  if (dialect.batch_rows == dialect::batch_source::value_constructors) {
    return values_row_source(entity, dialect, columns, row_count);
  }
  return fold_row_source(entity, dialect, columns, row_count) + " " +
         batch_rows_alias;
}

std::string folded_update_statement(entity_meta const& entity,
  dialect const& dialect, std::vector<std::size_t> const& set_col_indices,
  std::vector<std::size_t> const& where_col_indices, std::size_t row_count,
  fold_shape const& shape) {
  std::vector<std::size_t> const source_columns =
    concat_columns(where_col_indices, set_col_indices);
  std::string const source =
    fold_source(entity, dialect, source_columns, row_count, shape);
  std::string const table = entity.table_sql(dialect);
  switch (dialect.batch_write) {
  case dialect::batch_family::postgresql:
    // PostgreSQL names the written-to column bare in a SET: a qualifier there
    // is read as a column of the target, and the alias is not one.
    return "UPDATE " + table + " " + batch_target_alias + " SET " +
           fold_assignments(entity, dialect, set_col_indices, {}) + " FROM " +
           source + " WHERE " +
           fold_match(entity, dialect, where_col_indices, batch_target_alias,
             batch_rows_alias, " AND ");
  case dialect::batch_family::mysql:
    return "UPDATE " + table + " " + batch_target_alias + " JOIN " + source +
           " ON " +
           fold_match(entity, dialect, where_col_indices, batch_target_alias,
             batch_rows_alias, " AND ") +
           " SET " +
           fold_assignments(
             entity, dialect, set_col_indices, batch_target_alias);
  case dialect::batch_family::oracle:
    return "MERGE INTO " + table + " " + batch_target_alias + " USING " +
           source + " ON (" +
           fold_match(entity, dialect, where_col_indices, batch_target_alias,
             batch_rows_alias, " AND ") +
           ") WHEN MATCHED THEN UPDATE SET " +
           fold_assignments(
             entity, dialect, set_col_indices, batch_target_alias);
  case dialect::batch_family::sql_server:
    return "UPDATE " + batch_target_alias + " SET " +
           fold_assignments(
             entity, dialect, set_col_indices, batch_target_alias) +
           " FROM " + table + " " + batch_target_alias + " JOIN " + source +
           " ON " +
           fold_match(entity, dialect, where_col_indices, batch_target_alias,
             batch_rows_alias, " AND ");
  case dialect::batch_family::none:
    break;
  }
  return {};
}

std::string folded_delete_statement(entity_meta const& entity,
  dialect const& dialect, std::vector<std::size_t> const& where_col_indices,
  std::size_t row_count, fold_shape const& shape) {
  std::string const source =
    fold_source(entity, dialect, where_col_indices, row_count, shape);
  std::string const table = entity.table_sql(dialect);
  switch (dialect.batch_write) {
  case dialect::batch_family::postgresql:
    return "DELETE FROM " + table + " " + batch_target_alias + " USING " +
           source + " WHERE " +
           fold_match(entity, dialect, where_col_indices, batch_target_alias,
             batch_rows_alias, " AND ");
  case dialect::batch_family::mysql:
    return "DELETE " + batch_target_alias + " FROM " + table + " " +
           batch_target_alias + " JOIN " + source + " ON " +
           fold_match(entity, dialect, where_col_indices, batch_target_alias,
             batch_rows_alias, " AND ");
  case dialect::batch_family::oracle:
    return "DELETE FROM " + table + " " + batch_target_alias +
           " WHERE EXISTS ("
           "SELECT 1 FROM " +
           source + " WHERE " +
           fold_match(entity, dialect, where_col_indices, batch_rows_alias,
             batch_target_alias, " AND ") +
           ")";
  case dialect::batch_family::sql_server:
    return "DELETE " + batch_target_alias + " FROM " + table + " " +
           batch_target_alias + " JOIN " + source + " ON " +
           fold_match(entity, dialect, where_col_indices, batch_target_alias,
             batch_rows_alias, " AND ");
  case dialect::batch_family::none:
    break;
  }
  return {};
}

// A chunk of a folded batch must fit the server's parameter ceiling, which a
// row of this write costs `columns_per_row` of.
enum class fold_operation { update, remove };

std::string folded_statement(entity_meta const& entity, dialect const& dialect,
  fold_operation operation, std::vector<std::size_t> const& set_col_indices,
  std::vector<std::size_t> const& where_col_indices, std::size_t row_count,
  fold_shape const& shape) {
  return operation == fold_operation::update
           ? folded_update_statement(entity, dialect, set_col_indices,
               where_col_indices, row_count, shape)
           : folded_delete_statement(
               entity, dialect, where_col_indices, row_count, shape);
}

std::size_t fold_chunk(
  dialect const& dialect, std::size_t chunk, std::size_t columns_per_row) {
  if (columns_per_row == 0) {
    return 0;
  }
  std::size_t const by_ceiling =
    dialect.fold_parameter_ceiling / columns_per_row;
  return by_ceiling < chunk ? by_ceiling : chunk;
}

// The fold a sweep uses and how deep one execute of it goes. Arrays carry
// whatever the caller asked for: two parameters however many rows they hold,
// one text for every chunk, a parse as shallow as the column count. A tuple
// fold is cut to keep its parameters inside the ceiling the server's parser
// answers.
fold_plan plan_fold(connection& conn, entity_meta const& entity,
  dialect const& dialect, std::vector<std::size_t> const& columns,
  std::size_t asked_chunk) {
  fold_plan plan;
  plan.shape = choose_fold_shape(conn, entity, dialect, columns);
  plan.chunk = plan.shape.arrays
                 ? asked_chunk
                 : fold_chunk(dialect, asked_chunk, columns.size());
  return plan;
}

// One key's worth of bytes, so a batch that names the same row twice can be
// recognised. Every part carries its own tag and length, and the values a
// column can hold all fit: a NULL, a number, text or bytes, a point in time.
void append_key_part(
  std::string& out, sql_value const& value, std::size_t position) {
  out.push_back(static_cast<char>('a' + position % 26));
  std::visit(
    [&out](auto const& held) {
      using held_type = std::decay_t<decltype(held)>;
      if constexpr (std::is_same_v<held_type, std::monostate>) {
        out.push_back('\0');
      } else if constexpr (std::is_same_v<held_type, std::string>) {
        out.push_back('s');
        out.append(held);
        out.push_back('\0');
      } else if constexpr (std::is_same_v<held_type, std::vector<std::byte>>) {
        out.push_back('b');
        for (std::byte byte : held) {
          out.push_back(static_cast<char>(byte));
        }
        out.push_back('\0');
      } else if constexpr (std::is_same_v<held_type, timestamp>) {
        long long const ticks =
          static_cast<long long>(held.time_since_epoch().count());
        out.append(reinterpret_cast<char const*>(&ticks), sizeof(ticks));
      } else {
        // Numbers of one column arrive as one type, so a bool, an int16 and an
        // int64 of the same value never share a key here. Widening to the
        // widest form keeps that so.
        long long const widened = static_cast<long long>(held);
        out.append(reinterpret_cast<char const*>(&widened), sizeof(widened));
      }
    },
    value);
}

std::string row_key(entity_meta const& entity, void const* row,
  std::vector<std::size_t> const& key_col_indices) {
  std::string out;
  for (std::size_t position = 0; position < key_col_indices.size();
    ++position) {
    append_key_part(
      out, entity.columns[key_col_indices[position]].read(row), position);
  }
  return out;
}

// The rows a folded batch writes, with a key named twice kept once: the later
// row's values are the ones a per-row sweep leaves behind, and a server handed
// the same key twice picks neither. Rows come back in the order their first
// mention put them.
std::vector<std::size_t> distinct_key_rows(entity_meta const& entity,
  void const* rows, std::size_t row_stride, std::size_t row_count,
  std::vector<std::size_t> const& key_col_indices) {
  std::vector<std::size_t> order;
  order.reserve(row_count);
  std::unordered_map<std::string, std::size_t> seen;
  for (std::size_t index = 0; index < row_count; ++index) {
    void const* row = row_at(rows, row_stride, index);
    std::string key = row_key(entity, row, key_col_indices);
    auto const found = seen.find(key);
    if (found == seen.end()) {
      seen.emplace(std::move(key), order.size());
      order.push_back(index);
    } else {
      // The slot holds the row this key will be written with.
      order[found->second] = index;
    }
  }
  return order;
}

// One execute per chunk of the batch. Rows that go in as tuples make a
// statement that names as many of them as the chunk holds, so a short tail is
// its own text and its own cached statement; rows that go in as arrays share
// one text whatever the chunk holds.
std::size_t folded_batch_write(connection& conn, entity_meta const& entity,
  dialect const& dialect, fold_operation operation,
  std::vector<std::size_t> const& set_col_indices,
  std::vector<std::size_t> const& where_col_indices,
  std::vector<std::size_t> const& row_order, void const* rows,
  std::size_t row_stride, std::size_t chunk, fold_shape const& shape) {
  std::vector<std::size_t> const param_col_indices =
    operation == fold_operation::update
      ? concat_columns(where_col_indices, set_col_indices)
      : where_col_indices;

  std::size_t affected = 0;
  for (std::size_t start = 0; start < row_order.size(); start += chunk) {
    std::size_t const count = std::min(chunk, row_order.size() - start);
    std::string key = folded_statement(entity, dialect, operation,
      set_col_indices, where_col_indices, count, shape);
    auto stmt = conn.acquire_statement(key);
    // The backend binds pointers into this block rather than copying what it
    // is handed, so it is the rows' storage all the way to the execute below.
    // A chunk of arrays is one row of the same shape: one value per column.
    std::vector<params> batch;
    if (shape.arrays) {
      // Every column's array holds one element per row of the chunk, and the
      // walk that spells them is the only place the length comes from: an
      // array shorter than its neighbours is padded with NULLs by the server,
      // and a padded NULL overwrites what that row's column was to keep.
      std::vector<std::vector<std::string>> elements(param_col_indices.size());
      for (std::vector<std::string>& column : elements) {
        column.reserve(count);
      }
      for (std::size_t slot = 0; slot < count; ++slot) {
        std::vector<sql_value> const row_values = extract_row(entity,
          row_at(rows, row_stride, row_order[start + slot]), param_col_indices);
        for (std::size_t position = 0; position < param_col_indices.size();
          ++position) {
          elements[position].push_back(array_element(row_values.at(position),
            shape.type_names[param_col_indices[position]]));
        }
      }
      std::vector<sql_value> columns;
      columns.reserve(elements.size());
      for (auto const& column : elements) {
        columns.emplace_back(array_literal(column));
      }
      batch.emplace_back(std::move(columns));
    } else {
      batch.reserve(count);
      for (std::size_t slot = 0; slot < count; ++slot) {
        batch.emplace_back(
          extract_row(entity, row_at(rows, row_stride, row_order[start + slot]),
            param_col_indices));
      }
    }
    std::size_t ordinal = 1;
    // The row source lists a tuple as key columns then new values, and the
    // parameters follow that order row by row.
    for (auto const& row_params : batch) {
      for (std::size_t position = 0; position < row_params.size(); ++position) {
        stmt->bind_parameter(ordinal++, row_params.at(position));
      }
    }
    stmt->execute();
    affected += stmt->affected_rows();
    conn.release_statement(key, std::move(stmt));
  }
  return affected;
}

// What a batch adds up per execute(): the rows it bound, or what the driver
// reported. INSERT counts bound rows; an array-bound SQLExecute reports no
// per-parameter-set count.
enum class row_tally { bound_rows, affected_rows };

// --- Single-row statements: one execution, no array binding ---

std::size_t update_single_row(connection& conn, entity_meta const& m,
  std::vector<std::size_t> const& set_col_indices,
  std::vector<std::size_t> const& where_col_indices, void const* entity) {
  dialect const& d = conn.sql_dialect();
  std::vector<sql_value> values =
    extract_row(m, entity, concat_columns(set_col_indices, where_col_indices));
  return conn.execute_update(
    update_statement(m, d, set_col_indices, where_col_indices),
    params(std::move(values)));
}

std::size_t delete_single_row(connection& conn, entity_meta const& m,
  std::vector<std::size_t> const& where_col_indices, void const* entity) {
  dialect const& d = conn.sql_dialect();
  return conn.execute_update(delete_statement(m, d, where_col_indices),
    params(extract_row(m, entity, where_col_indices)));
}

// --- Rowwise batches: one block of row tuples bound per execute() ---

std::size_t update_rowwise(connection& conn, entity_meta const& m,
  std::vector<std::size_t> const& set_col_indices,
  std::vector<std::size_t> const& where_col_indices, void const* rows,
  std::size_t row_stride, std::size_t row_count, std::size_t batch_size) {
  dialect const& d = conn.sql_dialect();
  std::string sql = update_statement(m, d, set_col_indices, where_col_indices);
  std::vector<std::size_t> const param_cols =
    concat_columns(set_col_indices, where_col_indices);

  std::size_t affected = 0;
  std::string key(sql);
  auto stmt = conn.acquire_statement(key);

  for (std::size_t start = 0; start < row_count; start += batch_size) {
    std::size_t const count = std::min(batch_size, row_count - start);
    std::vector<params> batch;
    batch.reserve(count);
    for (std::size_t r = 0; r < count; ++r) {
      batch.emplace_back(
        extract_row(m, row_at(rows, row_stride, start + r), param_cols));
    }
    stmt->set_paramset_size(count);
    stmt->bind_batch_params(batch);
    stmt->execute();
    affected += stmt->affected_rows();
  }

  conn.release_statement(key, std::move(stmt));
  return affected;
}

std::size_t insert_rowwise(connection& conn, entity_meta const& m,
  void const* rows, std::size_t row_stride, std::size_t row_count,
  std::size_t batch_size) {
  dialect const& d = conn.sql_dialect();
  std::string sql = build_insert_sql(d, m);
  std::vector<std::size_t> const param_cols = all_columns(m);

  std::size_t inserted = 0;
  std::string key(sql);
  auto stmt = conn.acquire_statement(key);

  for (std::size_t start = 0; start < row_count; start += batch_size) {
    std::size_t const count = std::min(batch_size, row_count - start);
    std::vector<params> batch;
    batch.reserve(count);
    for (std::size_t r = 0; r < count; ++r) {
      batch.emplace_back(
        extract_row(m, row_at(rows, row_stride, start + r), param_cols));
    }
    stmt->bind_batch_params(batch);
    stmt->set_paramset_size(count);
    stmt->execute();
    // An array insert leaves no row unmatched, so this tally asks the driver
    // for nothing: it is the count the columnar channel of insert takes too,
    // and why no row-count flag reaches insert's chunk.
    inserted += count;
  }

  conn.release_statement(key, std::move(stmt));
  return inserted;
}

std::size_t delete_rowwise(connection& conn, entity_meta const& m,
  std::vector<std::size_t> const& where_col_indices, void const* rows,
  std::size_t row_stride, std::size_t row_count, std::size_t batch_size) {
  dialect const& d = conn.sql_dialect();
  std::string sql = delete_statement(m, d, where_col_indices);

  std::size_t affected = 0;
  std::string key(sql);
  auto stmt = conn.acquire_statement(key);

  for (std::size_t start = 0; start < row_count; start += batch_size) {
    std::size_t const count = std::min(batch_size, row_count - start);
    std::vector<params> batch;
    batch.reserve(count);
    for (std::size_t r = 0; r < count; ++r) {
      batch.emplace_back(
        extract_row(m, row_at(rows, row_stride, start + r), where_col_indices));
    }
    stmt->set_paramset_size(count);
    stmt->bind_batch_params(batch);
    stmt->execute();
    affected += stmt->affected_rows();
  }

  conn.release_statement(key, std::move(stmt));
  return affected;
}

// --- Columnar batch write, shared by INSERT / UPDATE / DELETE ---

// One typed buffer per parameter, refilled `batch_size` rows at a time against
// buffers bound for the whole sweep. `param_to_col[p]` feeds parameter p.
std::size_t columnar_batch_write(connection& conn, entity_meta const& m,
  std::string sql, std::vector<std::size_t> const& param_to_col,
  void const* rows, std::size_t row_stride, std::size_t row_count,
  std::size_t batch_size, row_tally tally) {
  if (row_count == 0) {
    return 0;
  }
  std::size_t const num_params = param_to_col.size();

  std::string key(std::move(sql));
  auto stmt = conn.acquire_statement(key);

  // Pre-scan for max string/binary size only if needed
  std::vector<std::size_t> max_sizes(num_params, 1);
  bool has_var = false;
  for (std::size_t p = 0; p < num_params; ++p) {
    auto bt = m.columns[param_to_col[p]].buffer_type;
    if (bt == backend::buffer_type::chars ||
        bt == backend::buffer_type::bytes) {
      has_var = true;
      break;
    }
  }
  if (has_var) {
    for (std::size_t p = 0; p < num_params; ++p) {
      auto const& col = m.columns[param_to_col[p]];
      if (col.buffer_type == backend::buffer_type::chars ||
          col.buffer_type == backend::buffer_type::bytes) {
        for (std::size_t r = 0; r < row_count; ++r) {
          max_sizes[p] = std::max(
            max_sizes[p], col.get_string_size(row_at(rows, row_stride, r)));
        }
      }
    }
  }

  // Allocate batch via backend's batch_writer — backend owns the buffers
  std::size_t const alloc_count = std::min(batch_size, row_count);
  auto& writer = stmt->prepare_batch();

  std::vector<std::size_t> col_indices(num_params);
  for (std::size_t p = 0; p < num_params; ++p) {
    auto btype = m.columns[param_to_col[p]].buffer_type;
    std::size_t elem_size = element_size_for(btype, max_sizes[p]);
    col_indices[p] = writer.add_column(btype, alloc_count, elem_size);
  }

  // Write the first batch of data BEFORE binding — some drivers (e.g.
  // MariaDB ODBC) may inspect buffer contents at SQLBindParameter time.
  std::size_t const first_count = alloc_count;
  for (std::size_t p = 0; p < num_params; ++p) {
    auto col_idx = col_indices[p];
    void* buf = writer.data(col_idx);
    std::size_t stride = writer.element_size(col_idx);
    auto* inds = writer.indicators(col_idx);
    std::size_t entity_col = param_to_col[p];
    for (std::size_t r = 0; r < first_count; ++r) {
      m.columns[entity_col].write_to_param_buffer(
        row_at(rows, row_stride, r), r, buf, stride, inds);
    }
  }

  // Bind once — buffer pointers are stable across batches
  writer.finish();
  stmt->set_paramset_size(first_count);
  stmt->execute();
  std::size_t written =
    tally == row_tally::bound_rows ? first_count : stmt->affected_rows();

  for (std::size_t start = batch_size; start < row_count; start += batch_size) {
    std::size_t count = std::min(batch_size, row_count - start);

    for (std::size_t p = 0; p < num_params; ++p) {
      auto col_idx = col_indices[p];
      void* buf = writer.data(col_idx);
      std::size_t stride = writer.element_size(col_idx);
      auto* inds = writer.indicators(col_idx);
      std::size_t entity_col = param_to_col[p];
      for (std::size_t r = 0; r < count; ++r) {
        m.columns[entity_col].write_to_param_buffer(
          row_at(rows, row_stride, start + r), r, buf, stride, inds);
      }
    }

    if (count != first_count) {
      stmt->set_paramset_size(count);
    }
    stmt->execute();
    written += tally == row_tally::bound_rows ? count : stmt->affected_rows();
  }

  conn.release_statement(key, std::move(stmt));
  return written;
}

// A sweep must not half-commit: on an autocommitting connection, wrap it in a
// transaction. In manual mode the chunks already join the caller's.
std::optional<transaction> begin_batch(connection& conn) {
  std::optional<transaction> txn;
  if (conn.autocommit()) {
    txn.emplace(conn.begin());
  }
  return txn;
}

// A driver that reports a single parameter set's count for an array execute
// gives no total to tally a sweep with, so each set has to go on its own.
std::size_t write_chunk(connection const& conn, std::size_t chunk) {
  return conn.caps().array_rowcount_totals ? chunk : 1;
}

// The route a batch update/remove takes and how deep one execute of it goes.
struct batch_route {
  bool folded = false;
  // The driver cannot execute an array of parameter sets and this server has
  // no folded shape, so each row goes on its own (PARAMSET_SIZE = 1).
  bool paramset_one = false;
  fold_shape shape;
  std::size_t chunk = 1;
};

batch_route choose_batch_route(connection& conn, entity_meta const& entity,
  dialect const& d, std::vector<std::size_t> const& columns,
  std::size_t asked_chunk, orm::batch_policy policy, bool array_ok) {
  batch_route route;
  bool const can_fold = d.batch_write != dialect::batch_family::none &&
                        fold_chunk(d, asked_chunk, columns.size()) > 0;
  if (policy == orm::batch_policy::columnar) {
    if (!array_ok) {
      throw uniorm_error(
        "batch write: this driver cannot execute an array of parameter sets");
    }
    // The caller chose the array-of-parameter-sets path and takes the count
    // the driver reports for it, so the whole chunk goes in one execute and
    // write_chunk's collapse-to-one is deliberately bypassed.
    route.chunk = asked_chunk;
    return route;
  }
  // Fold when the caller asks for it, when the array path is unusable, or when
  // the array would count short of the whole block or send it one set at a
  // time. An array that both counts and carries the whole block is cheaper.
  bool const want_fold = policy == orm::batch_policy::folded || !array_ok ||
                         !conn.caps().array_rowcount_totals ||
                         !conn.caps().array_single_send;
  if (want_fold && can_fold) {
    fold_plan const plan = plan_fold(conn, entity, d, columns, asked_chunk);
    route.folded = true;
    route.shape = plan.shape;
    route.chunk = plan.chunk;
    return route;
  }
  if (policy == orm::batch_policy::folded) {
    throw uniorm_error("batch write: this server has no folded batch shape");
  }
  if (!array_ok) {
    route.paramset_one = true;
    route.chunk = 1;
    return route;
  }
  route.chunk = write_chunk(conn, asked_chunk);
  return route;
}

}  // namespace

// --- Entity write entry points ---
// Called by the thin templates in orm.hpp, which are the only place the Entity
// type is needed; below here storage is a byte stride and columns are indices.

std::size_t orm::insert_impl(connection& conn, entity_meta const& m,
  void const* rows, std::size_t row_stride, std::size_t row_count,
  std::size_t batch_size) {
  if (row_count == 0) {
    return 0;
  }
  std::size_t const chunk = batch_size > 0 ? batch_size : default_paramset_size;

  std::optional<transaction> txn = begin_batch(conn);

  std::size_t inserted;
  if (!conn.caps().array_insert) {
    // The driver drops or rejects an array INSERT, so each row goes on its own.
    inserted = insert_rowwise(conn, m, rows, row_stride, row_count, 1);
  } else if (conn.caps().columnar_batch) {
    dialect const& d = conn.sql_dialect();
    inserted =
      columnar_batch_write(conn, m, build_insert_sql(d, m), all_columns(m),
        rows, row_stride, row_count, chunk, row_tally::bound_rows);
  } else {
    inserted = insert_rowwise(conn, m, rows, row_stride, row_count, chunk);
  }

  if (txn) {
    txn->commit();
  }
  return inserted;
}

std::size_t orm::update_single_impl(connection& conn, entity_meta const& m,
  void const* entity,
  std::optional<std::vector<std::string>> const& where_fields) {
  std::vector<std::size_t> const where_cols =
    detail::resolve_where(m, where_fields, "update");
  std::vector<std::size_t> const set_cols =
    detail::set_columns_of(m, where_cols);
  if (set_cols.empty()) {
    throw uniorm_error(
      "update: no columns to set (all mapped columns are in WHERE)");
  }
  return update_single_row(conn, m, set_cols, where_cols, entity);
}

std::size_t orm::update_batch_impl(connection& conn, entity_meta const& m,
  void const* rows, std::size_t row_stride, std::size_t row_count,
  std::optional<std::vector<std::string>> const& where_fields,
  std::size_t batch_size, batch_policy policy) {
  if (row_count == 0) {
    return 0;
  }
  std::vector<std::size_t> const where_cols =
    detail::resolve_where(m, where_fields, "update");
  std::vector<std::size_t> const set_cols =
    detail::set_columns_of(m, where_cols);
  if (set_cols.empty()) {
    throw uniorm_error(
      "update: no columns to set (all mapped columns are in WHERE)");
  }
  std::size_t const asked_chunk =
    batch_size > 0 ? batch_size : default_paramset_size;
  dialect const& d = conn.sql_dialect();
  std::vector<std::size_t> const source_cols =
    concat_columns(where_cols, set_cols);
  batch_route const route = choose_batch_route(conn, m, d, source_cols,
    asked_chunk, policy, conn.caps().array_update_delete);

  std::optional<transaction> txn = begin_batch(conn);

  std::size_t updated;
  if (route.folded) {
    updated = folded_batch_write(conn, m, d, fold_operation::update, set_cols,
      where_cols, distinct_key_rows(m, rows, row_stride, row_count, where_cols),
      rows, row_stride, route.chunk, route.shape);
  } else if (route.paramset_one) {
    updated = update_rowwise(
      conn, m, set_cols, where_cols, rows, row_stride, row_count, 1);
  } else if (conn.caps().columnar_batch) {
    updated = columnar_batch_write(conn, m,
      update_statement(m, d, set_cols, where_cols),
      concat_columns(set_cols, where_cols), rows, row_stride, row_count,
      route.chunk, row_tally::affected_rows);
  } else {
    updated = update_rowwise(
      conn, m, set_cols, where_cols, rows, row_stride, row_count, route.chunk);
  }

  if (txn) {
    txn->commit();
  }
  return updated;
}

std::size_t orm::delete_single_impl(connection& conn, entity_meta const& m,
  void const* entity,
  std::optional<std::vector<std::string>> const& where_fields) {
  std::vector<std::size_t> const where_cols =
    detail::resolve_where(m, where_fields, "remove");
  return delete_single_row(conn, m, where_cols, entity);
}

std::size_t orm::delete_batch_impl(connection& conn, entity_meta const& m,
  void const* rows, std::size_t row_stride, std::size_t row_count,
  std::optional<std::vector<std::string>> const& where_fields,
  std::size_t batch_size, batch_policy policy) {
  if (row_count == 0) {
    return 0;
  }
  std::vector<std::size_t> const where_cols =
    detail::resolve_where(m, where_fields, "remove");
  std::size_t const asked_chunk =
    batch_size > 0 ? batch_size : default_paramset_size;
  dialect const& d = conn.sql_dialect();
  batch_route const route = choose_batch_route(conn, m, d, where_cols,
    asked_chunk, policy, conn.caps().array_update_delete);

  std::optional<transaction> txn = begin_batch(conn);

  std::size_t deleted;
  if (route.folded) {
    deleted = folded_batch_write(conn, m, d, fold_operation::remove, {},
      where_cols, distinct_key_rows(m, rows, row_stride, row_count, where_cols),
      rows, row_stride, route.chunk, route.shape);
  } else if (route.paramset_one) {
    deleted =
      delete_rowwise(conn, m, where_cols, rows, row_stride, row_count, 1);
  } else if (conn.caps().columnar_batch) {
    deleted = columnar_batch_write(conn, m, delete_statement(m, d, where_cols),
      where_cols, rows, row_stride, row_count, route.chunk,
      row_tally::affected_rows);
  } else {
    deleted = delete_rowwise(
      conn, m, where_cols, rows, row_stride, row_count, route.chunk);
  }

  if (txn) {
    txn->commit();
  }
  return deleted;
}

}  // namespace uniorm
