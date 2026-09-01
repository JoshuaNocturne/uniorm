#include "uniorm/orm.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <unordered_map>
#include <utility>

#include <sql.h>
#include <sqlext.h>

#include "uniorm/backend/backend.hpp"
#include <uniorm/detail/connection.hpp>
#include "uniorm/dialect.hpp"
#include "uniorm/builder/builder.hpp"
#include "uniorm/result_set.hpp"
#include "uniorm/transaction.hpp"

namespace uniorm {

namespace {

struct schema_column {
  bool nullable = false;
};

std::unordered_map<std::string, schema_column> load_table_schema(
  backend::schema_metadata& md, std::string const& table) {
  std::unordered_map<std::string, schema_column> schema;
  for (auto const& c : md.table_columns(table)) {
    schema[c.name] = schema_column{c.nullable};
  }
  return schema;
}

}  // namespace

// --- Connection lifecycle ---

orm::orm(std::string_view connection_string)
  : pooled_conn_(
      connection_pool_registry::instance().acquire(std::string(connection_string))) {}

orm::orm(connection_pool& pool)
  : pooled_conn_(pool.acquire()) {}

orm::~orm() = default;

orm::orm(orm&&) noexcept = default;

orm& orm::operator=(orm&&) noexcept = default;

void orm::connect(std::string_view connection_string) {
  pooled_conn_ =
    connection_pool_registry::instance().acquire(std::string(connection_string));
}

void orm::disconnect() {
  ensure_connected();
  pooled_conn_.reset();  // returns connection to pool
}

void orm::ensure_connected() const {
  if (!pooled_conn_ || !pooled_conn_->get().is_open()) {
    throw uniorm_error("orm: not connected; call connect() first");
  }
}

// --- Validation ---

void orm::validate(validation_mode mode) {
  ensure_connected();
  auto* md = pooled_conn_->get().extension<backend::schema_metadata>();
  if (md == nullptr) {
    throw mapping_error(
      "schema validation requires a backend that exposes schema metadata");
  }
  for (auto const& [type, meta] : entities_) {
    auto schema = load_table_schema(*md, meta.table);
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

// --- Entity query entry point ---

query_gateway orm::query() {
  ensure_connected();
  return query_gateway(*this);
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

}  // namespace

update_builder::update_builder(orm& db, std::string table)
  : orm_(&db), table_(std::move(table)) {}

update_builder& update_builder::where(std::string_view clause, params p) {
  where_ = std::string(clause);
  where_params_ = std::move(p);
  return *this;
}

std::size_t update_builder::execute() {
  if (set_.empty()) {
    throw uniorm_error("update: no columns to set");
  }
  if (blank(where_)) {
    throw uniorm_error("update: refusing to execute without a WHERE clause");
  }
  dialect const d = dialect::detect(orm_->native_connection().dbms_name());
  std::string sql = "UPDATE " + d.quote_identifier(table_) + " SET ";
  std::vector<sql_value> values;
  values.reserve(set_.size() + where_params_.size());
  for (std::size_t i = 0; i < set_.size(); ++i) {
    if (i != 0) {
      sql += ", ";
    }
    sql += d.quote_identifier(set_[i].first) + " = ?";
    values.push_back(set_[i].second);
  }
  sql += " WHERE " + where_;
  auto const& wp = where_params_.values();
  values.insert(values.end(), wp.begin(), wp.end());
  return orm_->execute_update(sql, params(std::move(values)));
}

remove_builder::remove_builder(orm& db, std::string table)
  : orm_(&db), table_(std::move(table)) {}

remove_builder& remove_builder::where(std::string_view clause, params p) {
  where_ = std::string(clause);
  where_params_ = std::move(p);
  return *this;
}

std::size_t remove_builder::execute() {
  if (blank(where_)) {
    throw uniorm_error("remove: refusing to execute without a WHERE clause");
  }
  dialect const d = dialect::detect(orm_->native_connection().dbms_name());
  std::string sql =
    "DELETE FROM " + d.quote_identifier(table_) + " WHERE " + where_;
  return orm_->execute_update(sql, where_params_);
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

// --- paramset_size setter ---

void orm::paramset_size(std::size_t size) noexcept {
  paramset_size_ = size;
}

// --- Non-template impl helpers ---

namespace {

std::string build_insert_sql(
  dialect const& d, entity_meta const& m) {
  std::size_t const n = m.columns.size();
  std::string sql = "INSERT INTO " + d.quote_identifier(m.table) + " (";
  for (std::size_t i = 0; i < n; ++i) {
    if (i != 0) {
      sql += ", ";
    }
    sql += d.quote_identifier(m.columns[i].column);
  }
  sql += ") VALUES (?";
  for (std::size_t i = 1; i < n; ++i) {
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

bool has_variable_columns(entity_meta const& m) {
  for (auto const& col : m.columns) {
    if (col.buffer_type == backend::buffer_type::chars ||
        col.buffer_type == backend::buffer_type::bytes) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::size_t orm::update_single_impl(connection& conn,
  entity_meta const& m,
  std::vector<std::string> const& set_columns,
  std::vector<sql_value> const& set_values,
  std::vector<std::string> const& where_fields,
  std::vector<sql_value> const& where_values) {
  if (set_columns.empty()) {
    throw uniorm_error(
      "update: no columns to set (all mapped columns are in WHERE)");
  }
  for (auto const& field : where_fields) {
    if (!std::any_of(m.columns.begin(), m.columns.end(),
          [&](column_meta const& c) { return c.column == field; })) {
      throw uniorm_error("update: WHERE field '" + field + "' is not mapped");
    }
  }

  dialect const d = dialect::detect(conn.dbms_name());

  std::string where_sql;
  for (std::size_t i = 0; i < where_fields.size(); ++i) {
    auto const& field = where_fields[i];
    column_meta const* col = nullptr;
    for (auto const& c : m.columns) {
      if (c.column == field) {
        col = &c;
        break;
      }
    }
    if (i != 0) {
      where_sql += " AND ";
    }
    where_sql += d.quote_identifier(col->column) + " = ?";
  }

  std::string sql = "UPDATE " + d.quote_identifier(m.table) + " SET ";
  for (std::size_t i = 0; i < set_columns.size(); ++i) {
    if (i != 0) {
      sql += ", ";
    }
    sql += d.quote_identifier(set_columns[i]) + " = ?";
  }
  sql += " WHERE " + where_sql;

  std::vector<sql_value> all_values;
  all_values.reserve(set_values.size() + where_values.size());
  all_values.insert(all_values.end(), set_values.begin(), set_values.end());
  all_values.insert(all_values.end(), where_values.begin(), where_values.end());

  return conn.execute_update(sql, params(std::move(all_values)));
}

std::size_t orm::update_batch_impl(connection& conn,
  entity_meta const& m,
  std::vector<std::string> const& set_columns,
  std::vector<std::string> const& where_fields,
  std::vector<std::vector<sql_value>> const& rows,
  std::size_t default_batch_size) {
  if (set_columns.empty()) {
    throw uniorm_error(
      "update: no columns to set (all mapped columns are in WHERE)");
  }
  if (where_fields.empty()) {
    throw uniorm_error("update: no WHERE fields specified");
  }

  std::size_t const batch_size = default_batch_size > 0 ? default_batch_size : 1000;
  dialect const d = dialect::detect(conn.dbms_name());

  std::string sql = "UPDATE " + d.quote_identifier(m.table) + " SET ";
  for (std::size_t i = 0; i < set_columns.size(); ++i) {
    if (i != 0) {
      sql += ", ";
    }
    sql += d.quote_identifier(set_columns[i]) + " = ?";
  }
  sql += " WHERE ";
  for (std::size_t i = 0; i < where_fields.size(); ++i) {
    if (i != 0) {
      sql += " AND ";
    }
    sql += d.quote_identifier(where_fields[i]) + " = ?";
  }

  std::size_t affected = 0;
  std::string key(sql);
  auto stmt = conn.acquire_statement(key);

  for (std::size_t start = 0; start < rows.size(); start += batch_size) {
    std::size_t count = std::min(batch_size, rows.size() - start);
    std::vector<params> batch;
    batch.reserve(count);
    for (std::size_t r = 0; r < count; ++r) {
      batch.emplace_back(rows[start + r]);
    }
    stmt->set_paramset_size(count);
    stmt->bind_batch_params(batch);
    stmt->execute();
    affected += stmt->affected_rows();
  }

  conn.release_statement(key, std::move(stmt));
  return affected;
}

std::size_t orm::insert_rowwise_impl(connection& conn,
  entity_meta const& m,
  std::vector<std::vector<sql_value>> const& rows,
  std::size_t default_batch_size) {
  std::size_t const batch_size = default_batch_size > 0 ? default_batch_size : 1000;
  dialect const d = dialect::detect(conn.dbms_name());
  std::string sql = build_insert_sql(d, m);

  std::size_t inserted = 0;
  std::string key(sql);
  auto stmt = conn.acquire_statement(key);

  for (std::size_t start = 0; start < rows.size(); start += batch_size) {
    std::size_t count = std::min(batch_size, rows.size() - start);
    std::vector<params> batch;
    batch.reserve(count);
    for (std::size_t r = 0; r < count; ++r) {
      batch.emplace_back(rows[start + r]);
    }
    stmt->bind_batch_params(batch);
    stmt->set_paramset_size(count);
    stmt->execute();
    inserted += stmt->affected_rows();
  }

  conn.release_statement(key, std::move(stmt));
  return inserted;
}

// --- Columnar batch insert (non-template core) ---

std::size_t orm::insert_columnar_impl(connection& conn,
  entity_meta const& m, std::size_t row_count,
  std::size_t batch_size, void const* rows_data,
  string_size_fn string_size, write_row_fn write_row,
  void const* (*entity_at)(void const*, std::size_t)) {
  if (row_count == 0) {
    return 0;
  }

  std::size_t const num_cols = m.columns.size();

  dialect const d = dialect::detect(conn.dbms_name());
  std::string sql = build_insert_sql(d, m);

  std::string key(sql);
  auto stmt = conn.acquire_statement(key);

  // Pre-scan for max string/binary size only if needed
  std::vector<std::size_t> max_sizes(num_cols, 1);
  if (has_variable_columns(m)) {
    for (std::size_t c = 0; c < num_cols; ++c) {
      auto const& col = m.columns[c];
      if (col.buffer_type == backend::buffer_type::chars ||
          col.buffer_type == backend::buffer_type::bytes) {
        for (std::size_t r = 0; r < row_count; ++r) {
          void const* entity = entity_at(rows_data, r);
          max_sizes[c] = std::max(max_sizes[c], string_size(&m, entity, c));
        }
      }
    }
  }

  // Allocate batch via backend's batch_writer — backend owns the buffers
  std::size_t const alloc_count = std::min(batch_size, row_count);
  auto& writer = stmt->prepare_batch();

  std::vector<std::size_t> col_indices(num_cols);
  for (std::size_t c = 0; c < num_cols; ++c) {
    auto btype = m.columns[c].buffer_type;
    std::size_t elem_size = element_size_for(btype, max_sizes[c]);
    col_indices[c] = writer.add_column(btype, alloc_count, elem_size);
  }

  // Write the first batch of data BEFORE binding — some drivers (e.g.
  // MariaDB ODBC) may inspect buffer contents at SQLBindParameter time.
  std::size_t const first_count = std::min(batch_size, row_count);
  for (std::size_t c = 0; c < num_cols; ++c) {
    auto col_idx = col_indices[c];
    void* buf = writer.data(col_idx);
    std::size_t stride = writer.element_size(col_idx);
    auto* inds = writer.indicators(col_idx);
    for (std::size_t r = 0; r < first_count; ++r) {
      void const* entity = entity_at(rows_data, r);
      write_row(&m, entity, r, c, buf, stride, inds);
    }
  }

  // Bind once — buffer pointers are stable across batches
  writer.finish();
  stmt->set_paramset_size(first_count);
  stmt->execute();
  std::size_t inserted = first_count;

  for (std::size_t start = batch_size; start < row_count; start += batch_size) {
    std::size_t count = std::min(batch_size, row_count - start);

    for (std::size_t c = 0; c < num_cols; ++c) {
      auto col_idx = col_indices[c];
      void* buf = writer.data(col_idx);
      std::size_t stride = writer.element_size(col_idx);
      auto* inds = writer.indicators(col_idx);
      for (std::size_t r = 0; r < count; ++r) {
        void const* entity = entity_at(rows_data, start + r);
        write_row(&m, entity, r, c, buf, stride, inds);
      }
    }

    if (count != first_count) {
      stmt->set_paramset_size(count);
    }
    stmt->execute();
    inserted += count;
  }

  conn.release_statement(key, std::move(stmt));
  return inserted;
}

// --- Columnar batch update (non-template core) ---

std::size_t orm::update_columnar_impl(connection& conn,
  entity_meta const& m,
  std::vector<std::string> const& set_columns,
  std::vector<std::size_t> const& set_col_indices,
  std::vector<std::string> const& where_fields,
  std::vector<std::size_t> const& where_col_indices,
  std::size_t row_count, std::size_t batch_size,
  void const* rows_data,
  string_size_fn string_size, write_row_fn write_row,
  void const* (*entity_at)(void const*, std::size_t)) {
  if (row_count == 0) {
    return 0;
  }

  std::size_t const num_params = set_col_indices.size() + where_col_indices.size();
  std::vector<std::size_t> param_to_col(num_params);
  std::size_t pi = 0;
  for (auto ci : set_col_indices) {
    param_to_col[pi++] = ci;
  }
  for (auto ci : where_col_indices) {
    param_to_col[pi++] = ci;
  }

  dialect const d = dialect::detect(conn.dbms_name());
  std::string sql = "UPDATE " + d.quote_identifier(m.table) + " SET ";
  for (std::size_t i = 0; i < set_columns.size(); ++i) {
    if (i != 0) {
      sql += ", ";
    }
    sql += d.quote_identifier(set_columns[i]) + " = ?";
  }
  sql += " WHERE ";
  for (std::size_t i = 0; i < where_fields.size(); ++i) {
    if (i != 0) {
      sql += " AND ";
    }
    sql += d.quote_identifier(where_fields[i]) + " = ?";
  }

  std::string key(sql);
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
          void const* entity = entity_at(rows_data, r);
          max_sizes[p] = std::max(max_sizes[p], string_size(&m, entity, param_to_col[p]));
        }
      }
    }
  }

  std::size_t const alloc_count = std::min(batch_size, row_count);
  auto& writer = stmt->prepare_batch();

  std::vector<std::size_t> col_indices(num_params);
  for (std::size_t p = 0; p < num_params; ++p) {
    auto btype = m.columns[param_to_col[p]].buffer_type;
    std::size_t elem_size = element_size_for(btype, max_sizes[p]);
    col_indices[p] = writer.add_column(btype, alloc_count, elem_size);
  }

  std::size_t const first_count = alloc_count;
  for (std::size_t p = 0; p < num_params; ++p) {
    auto col_idx = col_indices[p];
    void* buf = writer.data(col_idx);
    std::size_t stride = writer.element_size(col_idx);
    auto* inds = writer.indicators(col_idx);
    std::size_t entity_col = param_to_col[p];
    for (std::size_t r = 0; r < first_count; ++r) {
      void const* entity = entity_at(rows_data, r);
      write_row(&m, entity, r, entity_col, buf, stride, inds);
    }
  }

  // Bind once — buffer pointers are stable across batches
  writer.finish();
  stmt->set_paramset_size(first_count);
  stmt->execute();
  std::size_t updated = stmt->affected_rows();

  for (std::size_t start = batch_size; start < row_count; start += batch_size) {
    std::size_t count = std::min(batch_size, row_count - start);

    for (std::size_t p = 0; p < num_params; ++p) {
      auto col_idx = col_indices[p];
      void* buf = writer.data(col_idx);
      std::size_t stride = writer.element_size(col_idx);
      auto* inds = writer.indicators(col_idx);
      std::size_t entity_col = param_to_col[p];
      for (std::size_t r = 0; r < count; ++r) {
        void const* entity = entity_at(rows_data, start + r);
        write_row(&m, entity, r, entity_col, buf, stride, inds);
      }
    }

    if (count != first_count) {
      stmt->set_paramset_size(count);
    }
    stmt->execute();
    updated += stmt->affected_rows();
  }

  conn.release_statement(key, std::move(stmt));
  return updated;
}

// --- Delete operations ---

std::size_t orm::delete_single_impl(connection& conn,
  entity_meta const& m,
  std::vector<std::string> const& where_fields,
  std::vector<sql_value> const& where_values) {
  if (where_fields.empty()) {
    throw uniorm_error("remove: no WHERE fields specified");
  }

  dialect const d = dialect::detect(conn.dbms_name());

  std::string sql = "DELETE FROM " + d.quote_identifier(m.table) + " WHERE ";
  for (std::size_t i = 0; i < where_fields.size(); ++i) {
    if (i != 0) {
      sql += " AND ";
    }
    sql += d.quote_identifier(where_fields[i]) + " = ?";
  }

  return conn.execute_update(sql, params(where_values));
}

std::size_t orm::delete_batch_impl(connection& conn,
  entity_meta const& m,
  std::vector<std::string> const& where_fields,
  std::vector<std::vector<sql_value>> const& rows,
  std::size_t default_batch_size) {
  if (where_fields.empty()) {
    throw uniorm_error("remove: no WHERE fields specified");
  }

  std::size_t const batch_size = default_batch_size > 0 ? default_batch_size : 1000;
  dialect const d = dialect::detect(conn.dbms_name());

  std::string sql = "DELETE FROM " + d.quote_identifier(m.table) + " WHERE ";
  for (std::size_t i = 0; i < where_fields.size(); ++i) {
    if (i != 0) {
      sql += " AND ";
    }
    sql += d.quote_identifier(where_fields[i]) + " = ?";
  }

  std::size_t affected = 0;
  std::string key(sql);
  auto stmt = conn.acquire_statement(key);

  for (std::size_t start = 0; start < rows.size(); start += batch_size) {
    std::size_t count = std::min(batch_size, rows.size() - start);
    std::vector<params> batch;
    batch.reserve(count);
    for (std::size_t r = 0; r < count; ++r) {
      batch.emplace_back(rows[start + r]);
    }
    stmt->set_paramset_size(count);
    stmt->bind_batch_params(batch);
    stmt->execute();
    affected += stmt->affected_rows();
  }

  conn.release_statement(key, std::move(stmt));
  return affected;
}

std::size_t orm::delete_columnar_impl(connection& conn,
  entity_meta const& m,
  std::vector<std::string> const& where_fields,
  std::vector<std::size_t> const& where_col_indices,
  std::size_t row_count, std::size_t batch_size,
  void const* rows_data,
  string_size_fn string_size, write_row_fn write_row,
  void const* (*entity_at)(void const* data, std::size_t i)) {
  if (row_count == 0) {
    return 0;
  }

  std::size_t const num_params = where_col_indices.size();
  std::vector<std::size_t> param_to_col(num_params);
  std::size_t pi = 0;
  for (auto ci : where_col_indices) {
    param_to_col[pi++] = ci;
  }

  dialect const d = dialect::detect(conn.dbms_name());
  std::string sql = "DELETE FROM " + d.quote_identifier(m.table) + " WHERE ";
  for (std::size_t i = 0; i < where_fields.size(); ++i) {
    if (i != 0) {
      sql += " AND ";
    }
    sql += d.quote_identifier(where_fields[i]) + " = ?";
  }

  std::string key(sql);
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
          void const* entity = entity_at(rows_data, r);
          max_sizes[p] = std::max(max_sizes[p], string_size(&m, entity, param_to_col[p]));
        }
      }
    }
  }

  std::size_t const alloc_count = std::min(batch_size, row_count);
  auto& writer = stmt->prepare_batch();

  std::vector<std::size_t> col_indices(num_params);
  for (std::size_t p = 0; p < num_params; ++p) {
    auto btype = m.columns[param_to_col[p]].buffer_type;
    std::size_t elem_size = element_size_for(btype, max_sizes[p]);
    col_indices[p] = writer.add_column(btype, alloc_count, elem_size);
  }

  std::size_t const first_count = alloc_count;
  for (std::size_t p = 0; p < num_params; ++p) {
    auto col_idx = col_indices[p];
    void* buf = writer.data(col_idx);
    std::size_t stride = writer.element_size(col_idx);
    auto* inds = writer.indicators(col_idx);
    std::size_t entity_col = param_to_col[p];
    for (std::size_t r = 0; r < first_count; ++r) {
      void const* entity = entity_at(rows_data, r);
      write_row(&m, entity, r, entity_col, buf, stride, inds);
    }
  }

  // Bind once — buffer pointers are stable across batches
  writer.finish();
  stmt->set_paramset_size(first_count);
  stmt->execute();
  std::size_t deleted = stmt->affected_rows();

  for (std::size_t start = batch_size; start < row_count; start += batch_size) {
    std::size_t count = std::min(batch_size, row_count - start);

    for (std::size_t p = 0; p < num_params; ++p) {
      auto col_idx = col_indices[p];
      void* buf = writer.data(col_idx);
      std::size_t stride = writer.element_size(col_idx);
      auto* inds = writer.indicators(col_idx);
      std::size_t entity_col = param_to_col[p];
      for (std::size_t r = 0; r < count; ++r) {
        void const* entity = entity_at(rows_data, start + r);
        write_row(&m, entity, r, entity_col, buf, stride, inds);
      }
    }

    if (count != first_count) {
      stmt->set_paramset_size(count);
    }
    stmt->execute();
    deleted += stmt->affected_rows();
  }

  conn.release_statement(key, std::move(stmt));
  return deleted;
}

}  // namespace uniorm
