#include "uniorm/orm.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <utility>

#include "orm_mapping.hpp"
#include "uniorm/backend/backend.hpp"
#include <uniorm/connection.hpp>
#include "uniorm/dialect.hpp"
#include "uniorm/builder/builder.hpp"
#include "uniorm/result_set.hpp"
#include "uniorm/transaction.hpp"

namespace uniorm {

namespace {

// Begin a lease in the mode its new owner asked for, discarding any pending
// work a previous lease left behind: enabling autocommit would commit it.
void adopt_connection(
  connection& conn, bool autocommit, dialect::identifier_case identifiers) {
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

orm::orm(connection_pool& pool) : pooled_conn_(pool.acquire()) {
  adopt_connection(native_connection(), auto_commit_, identifiers_);
}

orm::~orm() = default;

orm::orm(orm&&) noexcept = default;

orm& orm::operator=(orm&&) noexcept = default;

void orm::connect(std::string_view connection_string) {
  clear_identifier_resolution();
  pooled_conn_ =
    connection_pool_registry::instance().acquire(std::string(connection_string));
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
  static_assert(std::is_nothrow_swappable_v<
    std::optional<identifier_resolution>>);
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
  std::string sql = "DELETE FROM " + d.quote_identifier(table_) +
                    " WHERE " + where;
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

dialect::identifier_case orm::identifier_case() const noexcept {
  return identifiers_;
}

void orm::identifier_case(dialect::identifier_case policy) {
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

std::size_t element_size_for(backend::buffer_type type, std::size_t max_var_size) {
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
  std::vector<std::size_t> const& head,
  std::vector<std::size_t> const& tail) {
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
std::string update_statement(entity_meta const& entity,
  dialect const& dialect, std::vector<std::size_t> const& set_col_indices,
  std::vector<std::size_t> const& where_col_indices) {
  return "UPDATE " + entity.table_sql(dialect) + " SET " +
    placeholder_group(entity, set_col_indices, dialect, ", ") + " WHERE " +
    placeholder_group(entity, where_col_indices, dialect, " AND ");
}

std::string delete_statement(entity_meta const& entity,
  dialect const& dialect,
  std::vector<std::size_t> const& where_col_indices) {
  return "DELETE FROM " + entity.table_sql(dialect) + " WHERE " +
    placeholder_group(entity, where_col_indices, dialect, " AND ");
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
  std::vector<sql_value> values = extract_row(
    m, entity, concat_columns(set_col_indices, where_col_indices));
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
  std::string sql =
    update_statement(m, d, set_col_indices, where_col_indices);
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
      batch.emplace_back(extract_row(
        m, row_at(rows, row_stride, start + r), param_cols));
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
      batch.emplace_back(extract_row(
        m, row_at(rows, row_stride, start + r), param_cols));
    }
    stmt->bind_batch_params(batch);
    stmt->set_paramset_size(count);
    stmt->execute();
    inserted += stmt->affected_rows();
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
      batch.emplace_back(extract_row(
        m, row_at(rows, row_stride, start + r), where_col_indices));
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
    if (bt == backend::buffer_type::chars || bt == backend::buffer_type::bytes) {
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
          max_sizes[p] = std::max(max_sizes[p],
            col.get_string_size(row_at(rows, row_stride, r)));
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
    written +=
      tally == row_tally::bound_rows ? count : stmt->affected_rows();
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
  if (conn.caps().columnar_batch) {
    dialect const& d = conn.sql_dialect();
    inserted = columnar_batch_write(conn, m, build_insert_sql(d, m),
      all_columns(m), rows, row_stride, row_count, chunk,
      row_tally::bound_rows);
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
  std::size_t batch_size) {
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
  std::size_t const chunk =
    write_chunk(conn, batch_size > 0 ? batch_size : default_paramset_size);

  std::optional<transaction> txn = begin_batch(conn);

  std::size_t updated;
  if (conn.caps().columnar_batch) {
    dialect const& d = conn.sql_dialect();
    updated = columnar_batch_write(conn, m,
      update_statement(m, d, set_cols, where_cols),
      concat_columns(set_cols, where_cols), rows, row_stride, row_count, chunk,
      row_tally::affected_rows);
  } else {
    updated = update_rowwise(conn, m, set_cols, where_cols, rows, row_stride,
      row_count, chunk);
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
  std::size_t batch_size) {
  if (row_count == 0) {
    return 0;
  }
  std::vector<std::size_t> const where_cols =
    detail::resolve_where(m, where_fields, "remove");
  std::size_t const chunk =
    write_chunk(conn, batch_size > 0 ? batch_size : default_paramset_size);

  std::optional<transaction> txn = begin_batch(conn);

  std::size_t deleted;
  if (conn.caps().columnar_batch) {
    dialect const& d = conn.sql_dialect();
    deleted = columnar_batch_write(conn, m,
      delete_statement(m, d, where_cols), where_cols, rows, row_stride,
      row_count, chunk, row_tally::affected_rows);
  } else {
    deleted =
      delete_rowwise(conn, m, where_cols, rows, row_stride, row_count, chunk);
  }

  if (txn) {
    txn->commit();
  }
  return deleted;
}

}  // namespace uniorm
