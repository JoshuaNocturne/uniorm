#pragma once

// Central entry point for uniorm: owns a database connection and entity
// mappings, provides all database operations.

#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/detail/connection.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/pool.hpp>
#include <uniorm/transaction.hpp>

namespace uniorm {

class result_set;
class transaction;
class update_builder;
class remove_builder;
class query_gateway;
template <class T>
class query;

// Central entry point for uniorm: owns a database connection and entity
// mappings, provides all database operations.
class UNIORM_API orm {
public:
  orm() = default;
  explicit orm(std::string_view connection_string);
  explicit orm(connection_pool& pool);  // Acquire connection from user-managed pool
  ~orm();

  orm(orm&&) noexcept;
  orm& operator=(orm&&) noexcept;

  orm(orm const&) = delete;
  orm& operator=(orm const&) = delete;

  // --- Connection lifecycle ---
  void connect(std::string_view connection_string);
  void disconnect();

  // --- Block fetching configuration ---
  std::size_t row_array_size() const noexcept { return row_array_size_; }
  void row_array_size(std::size_t size) noexcept { row_array_size_ = size; }

  // --- Batch operation configuration (insert/update/delete) ---
  std::size_t paramset_size() const noexcept { return paramset_size_; }
  void paramset_size(std::size_t size) noexcept {
    paramset_size_ = size;
    if (pooled_conn_) {
      pooled_conn_->get().paramset_size(size);
    }
  }

  // --- Entity mapping ---
  template <class T>
  mapping_builder<T> map(std::string_view table) {
    std::type_index type(typeid(T));
    if (entities_.count(type) != 0) {
      throw mapping_error(
        std::string("entity already registered: ") + typeid(T).name());
    }
    entity_meta& meta = entities_[type];
    meta.table = table;
    return mapping_builder<T>(meta);
  }

  template <class T>
  entity_meta const& meta() const {
    auto it = entities_.find(std::type_index(typeid(T)));
    if (it == entities_.end()) {
      throw mapping_error(
        std::string("entity not registered: ") + typeid(T).name());
    }
    return it->second;
  }

  entity_meta const* find(std::type_index type) const {
    auto it = entities_.find(type);
    return it == entities_.end() ? nullptr : &it->second;
  }

  std::size_t size() const noexcept {
    return entities_.size();
  }

  // Validate mappings against live schema.
  void validate(validation_mode mode = validation_mode::strict);

  // --- Entity operations ---
  template <class Entity>
  std::size_t insert(std::vector<Entity> const& rows) {
    ensure_connected();
    entity_meta const& m = meta<Entity>();

    // Use direct buffer path to avoid sql_value overhead
    return insert_entities_direct<Entity>(m, rows);
  }

private:
  // Direct entity insert: writes entity fields directly to ODBC parameter
  // buffers, bypassing the sql_value variant intermediate layer.
  template <class Entity>
  std::size_t insert_entities_direct(
    entity_meta const& m, std::vector<Entity> const& rows) {
    if (rows.empty()) {
      return 0;
    }

    std::size_t const num_cols = m.columns.size();
    std::size_t const batch_size = paramset_size_ > 0 ? paramset_size_ : 1000;

    // Build SQL
    dialect const d = dialect::detect(conn().dbms_name());
    std::string sql = "INSERT INTO " + d.quote_identifier(m.table) + " (";
    for (std::size_t i = 0; i < num_cols; ++i) {
      if (i != 0) {
        sql += ", ";
      }
      sql += d.quote_identifier(m.columns[i].column);
    }
    sql += ") VALUES (?";
    for (std::size_t i = 1; i < num_cols; ++i) {
      sql += ", ?";
    }
    sql += ")";

    // Column buffer structure for direct binding
    struct col_buffer {
      backend::buffer_type type = backend::buffer_type::chars;
      std::vector<unsigned char> bit_vals;
      std::vector<std::int16_t> i16_vals;
      std::vector<std::int32_t> i32_vals;
      std::vector<std::int64_t> i64_vals;
      std::vector<double> f64_vals;
      std::vector<backend::timestamp_parts> ts_vals;
      std::vector<char> var_buf;
      std::size_t stride = 0;
      std::vector<std::int64_t> indicators;
    };

    std::vector<col_buffer> cols(num_cols);
    std::size_t inserted = 0;
    transaction txn = conn().begin();

    std::string key(sql);
    auto stmt = conn().acquire_cached(key);

    // Pre-scan ALL rows for max string/binary size once (not per batch)
    std::vector<std::size_t> max_sizes(num_cols, 1);
    for (std::size_t c = 0; c < num_cols; ++c) {
      auto const& col = m.columns[c];
      if (col.buffer_type == backend::buffer_type::chars ||
          col.buffer_type == backend::buffer_type::bytes) {
        for (std::size_t r = 0; r < rows.size(); ++r) {
          max_sizes[c] = std::max(max_sizes[c], col.get_string_size(&rows[r]));
        }
      }
    }

    // Pre-allocate buffers with max batch size — pointers stay stable
    std::size_t const alloc_count = std::min(batch_size, rows.size());
    for (std::size_t c = 0; c < num_cols; ++c) {
      auto& col_buf = cols[c];
      col_buf.type = m.columns[c].buffer_type;
      col_buf.indicators.resize(alloc_count);

      switch (col_buf.type) {
      case backend::buffer_type::bit:
        col_buf.bit_vals.resize(alloc_count, 0);
        col_buf.stride = sizeof(unsigned char);
        break;
      case backend::buffer_type::int16:
        col_buf.i16_vals.resize(alloc_count, 0);
        col_buf.stride = sizeof(std::int16_t);
        break;
      case backend::buffer_type::int32:
        col_buf.i32_vals.resize(alloc_count, 0);
        col_buf.stride = sizeof(std::int32_t);
        break;
      case backend::buffer_type::int64:
        col_buf.i64_vals.resize(alloc_count, 0);
        col_buf.stride = sizeof(std::int64_t);
        break;
      case backend::buffer_type::float64:
        col_buf.f64_vals.resize(alloc_count, 0.0);
        col_buf.stride = sizeof(double);
        break;
      case backend::buffer_type::chars:
      case backend::buffer_type::bytes:
        col_buf.stride = std::max(max_sizes[c] + 1, std::size_t(65));
        col_buf.var_buf.resize(alloc_count * col_buf.stride, '\0');
        break;
      case backend::buffer_type::timestamp_parts:
        col_buf.ts_vals.resize(alloc_count, {});
        col_buf.stride = sizeof(backend::timestamp_parts);
        break;
      default:
        break;
      }
    }

    // Bind ONCE before the loop — buffer pointers are stable
    for (std::size_t c = 0; c < num_cols; ++c) {
      auto& col_buf = cols[c];
      backend::param_array_buffer pab{};
      pab.type = col_buf.type;
      pab.indicators = col_buf.indicators.data();
      pab.count = alloc_count;

      switch (col_buf.type) {
      case backend::buffer_type::bit:
        pab.data = col_buf.bit_vals.data();
        pab.stride = sizeof(unsigned char);
        break;
      case backend::buffer_type::int16:
        pab.data = col_buf.i16_vals.data();
        pab.stride = sizeof(std::int16_t);
        break;
      case backend::buffer_type::int32:
        pab.data = col_buf.i32_vals.data();
        pab.stride = sizeof(std::int32_t);
        break;
      case backend::buffer_type::int64:
        pab.data = col_buf.i64_vals.data();
        pab.stride = sizeof(std::int64_t);
        break;
      case backend::buffer_type::float64:
        pab.data = col_buf.f64_vals.data();
        pab.stride = sizeof(double);
        break;
      case backend::buffer_type::chars:
      case backend::buffer_type::bytes:
        pab.data = col_buf.var_buf.data();
        pab.stride = col_buf.stride;
        break;
      case backend::buffer_type::timestamp_parts:
        pab.data = col_buf.ts_vals.data();
        pab.stride = sizeof(backend::timestamp_parts);
        break;
      default:
        break;
      }

      stmt->bind_param_array(c + 1, pab);
    }

    // Set paramset size once before the loop
    stmt->set_paramset_size(alloc_count);

    for (std::size_t start = 0; start < rows.size(); start += batch_size) {
      std::size_t end = std::min(start + batch_size, rows.size());
      std::size_t count = end - start;

      // Update paramset size only for last batch if different
      if (count != alloc_count) {
        stmt->set_paramset_size(count);
      }

      // Write entity fields directly to pre-allocated buffers
      for (std::size_t c = 0; c < num_cols; ++c) {
        auto& col_buf = cols[c];
        auto const& col_meta = m.columns[c];

        void* buffer = nullptr;
        switch (col_buf.type) {
        case backend::buffer_type::bit:
          buffer = col_buf.bit_vals.data();
          break;
        case backend::buffer_type::int16:
          buffer = col_buf.i16_vals.data();
          break;
        case backend::buffer_type::int32:
          buffer = col_buf.i32_vals.data();
          break;
        case backend::buffer_type::int64:
          buffer = col_buf.i64_vals.data();
          break;
        case backend::buffer_type::float64:
          buffer = col_buf.f64_vals.data();
          break;
        case backend::buffer_type::chars:
        case backend::buffer_type::bytes:
          buffer = col_buf.var_buf.data();
          break;
        case backend::buffer_type::timestamp_parts:
          buffer = col_buf.ts_vals.data();
          break;
        default:
          break;
        }

        for (std::size_t r = 0; r < count; ++r) {
          col_meta.write_to_param_buffer(
            &rows[start + r], r, buffer, col_buf.stride, col_buf.indicators.data());
        }
      }

      stmt->execute();
      inserted += count;
    }

    conn().stmt_cache_->release(key, std::move(stmt));
    txn.commit();
    return inserted;
  }

public:

  template <class Entity>
    requires(!std::is_convertible_v<Entity const&, std::string_view>)
  std::size_t update(Entity const& entity) {
    ensure_connected();
    entity_meta const& m = meta<Entity>();
    std::string pk;
    for (auto const& c : m.columns) {
      if (c.is_primary_key) {
        pk = c.column;
        break;
      }
    }
    if (pk.empty()) {
      throw uniorm_error(
        "update: entity has no primary key; specify where_fields explicitly");
    }
    return update(entity, std::vector<std::string>{ pk });
  }

  template <class Entity>
    requires(!std::is_convertible_v<Entity const&, std::string_view>)
  std::size_t update(Entity const& entity,
    std::vector<std::string> const& where_fields) {
    ensure_connected();
    if (where_fields.empty()) {
      throw uniorm_error("update: no WHERE fields specified");
    }
    entity_meta const& m = meta<Entity>();

    std::set<std::string> where_set(where_fields.begin(), where_fields.end());
    std::vector<std::string> set_columns;
    std::vector<sql_value> set_values;
    for (auto const& c : m.columns) {
      if (where_set.find(c.column) == where_set.end()) {
        set_columns.push_back(c.column);
        set_values.push_back(c.read(&entity));
      }
    }

    if (set_columns.empty()) {
      throw uniorm_error(
        "update: no columns to set (all mapped columns are in WHERE)");
    }

    dialect const d = dialect::detect(dbms_name());

    std::vector<sql_value> where_values;
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
      if (!col) {
        throw uniorm_error("update: WHERE field '" + field + "' is not mapped");
      }
      if (i != 0) {
        where_sql += " AND ";
      }
      where_sql += d.quote_identifier(col->column) + " = ?";
      where_values.push_back(col->read(&entity));
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

    return pooled_conn_->get().execute_update(sql, params(std::move(all_values)));
  }

  template <class Entity>
  std::size_t update(std::vector<Entity> const& entities) {
    ensure_connected();
    entity_meta const& m = meta<Entity>();
    std::string pk;
    for (auto const& c : m.columns) {
      if (c.is_primary_key) {
        pk = c.column;
        break;
      }
    }
    if (pk.empty()) {
      throw uniorm_error(
        "update: entity has no primary key; specify where_fields explicitly");
    }
    return update(entities, std::vector<std::string>{ pk });
  }

  template <class Entity>
  std::size_t update(std::vector<Entity> const& entities,
    std::vector<std::string> const& where_fields) {
    ensure_connected();
    if (entities.empty()) {
      return 0;
    }
    if (where_fields.empty()) {
      throw uniorm_error("update: no WHERE fields specified");
    }
    entity_meta const& m = meta<Entity>();

    std::set<std::string> where_set(where_fields.begin(), where_fields.end());
    std::vector<std::string> set_columns;
    for (auto const& c : m.columns) {
      if (where_set.find(c.column) == where_set.end()) {
        set_columns.push_back(c.column);
      }
    }

    if (set_columns.empty()) {
      throw uniorm_error(
        "update: no columns to set (all mapped columns are in WHERE)");
    }

    // Build parameter rows: [set_col1, set_col2, ..., where_col1, where_col2, ...]
    std::vector<params> rows;
    rows.reserve(entities.size());
    for (auto const& entity : entities) {
      std::vector<sql_value> row_values;
      row_values.reserve(set_columns.size() + where_fields.size());
      
      // SET columns
      for (auto const& col_name : set_columns) {
        for (auto const& c : m.columns) {
          if (c.column == col_name) {
            row_values.push_back(c.read(&entity));
            break;
          }
        }
      }
      
      // WHERE columns
      for (auto const& field : where_fields) {
        for (auto const& c : m.columns) {
          if (c.column == field) {
            row_values.push_back(c.read(&entity));
            break;
          }
        }
      }
      
      rows.emplace_back(std::move(row_values));
    }

    return pooled_conn_->get().update_batch(m.table, set_columns, where_fields, rows);
  }

  // --- Entity query entry point ---
  query_gateway query();

  // --- Raw SQL operations ---
  result_set execute(std::string_view sql, params const& p = {});
  std::size_t execute_update(std::string_view sql, params const& p = {});

  template <detail::aggregate_projection T>
  std::vector<T> query(std::string_view sql, params const& p = {}) {
    ensure_connected();
    return pooled_conn_->get().query<T>(sql, p);
  }

  // --- Dynamic (non-entity) operations ---
  std::size_t insert_batch(std::string_view table,
    std::vector<std::string> const& columns, std::vector<params> const& rows);

  update_builder update(std::string_view table);
  remove_builder remove(std::string_view table);

  // --- Transaction ---
  transaction begin();
  void commit();
  void rollback();

  // --- Cache observability ---
  unsigned long long statement_cache_hits() const;
  unsigned long long statement_cache_misses() const;
  std::size_t statement_cache_size() const;
  void clear_statement_cache();

  // --- Escape hatches ---
  template <class T>
  T* native_handle() noexcept {
    return pooled_conn_ ? pooled_conn_->get().native_handle<T>() : nullptr;
  }

  template <class T>
  T* extension() noexcept {
    return pooled_conn_ ? pooled_conn_->get().extension<T>() : nullptr;
  }

private:
  friend class query_gateway;

  connection& conn() {
    ensure_connected();
    return pooled_conn_->get();
  }

  std::string dbms_name() const {
    ensure_connected();
    return pooled_conn_->get().dbms_name();
  }

  std::optional<pooled_connection> pooled_conn_;
  std::unordered_map<std::type_index, entity_meta> entities_;
  std::size_t row_array_size_ = 100;  // Default block fetch size
  std::size_t paramset_size_ = 1000;  // Default batch insert size

  void ensure_connected() const;
};

}  // namespace uniorm
