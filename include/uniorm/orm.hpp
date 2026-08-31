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
#include <uniorm/detail/projection.hpp>
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
  // Acquire connection from user-managed pool
  explicit orm(connection_pool& pool);
  ~orm();

  orm(orm&&) noexcept;
  orm& operator=(orm&&) noexcept;

  orm(orm const&) = delete;
  orm& operator=(orm const&) = delete;

  // --- Connection lifecycle ---
  void connect(std::string_view connection_string);
  void disconnect();

  // --- Block fetching configuration ---
  std::size_t row_array_size() const noexcept {
    return row_array_size_;
  }
  void row_array_size(std::size_t size) noexcept {
    row_array_size_ = size;
  }

  // --- Batch operation configuration (insert/update) ---
  std::size_t paramset_size() const noexcept {
    return paramset_size_;
  }
  void paramset_size(std::size_t size) noexcept;

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

  // ========================================================================
  // CREATE (Insert)
  // ========================================================================

  template <class Entity>
  std::size_t insert(std::vector<Entity> const& rows) {
    ensure_connected();
    entity_meta const& m = meta<Entity>();
    std::optional<transaction> txn;
    if (auto_commit_) txn.emplace(native_connection().begin());
    std::size_t result;
    if (native_connection().caps().columnar_batch) {
      auto string_size = +[](void const* meta, void const* entity,
                          std::size_t col) -> std::size_t {
        return static_cast<entity_meta const*>(meta)
          ->columns[col]
          .get_string_size(entity);
      };
      auto write_row = +[](void const* meta, void const* entity,
                        std::size_t row, std::size_t col, void* buf,
                        std::size_t stride, std::int64_t* inds) {
        static_cast<entity_meta const*>(meta)
          ->columns[col]
          .write_to_param_buffer(entity, row, buf, stride, inds);
      };
      auto entity_at = +[](void const* data,
                        std::size_t i) -> void const* {
        return &(*static_cast<std::vector<Entity> const*>(data))[i];
      };
      result = insert_columnar_impl(native_connection(), m, rows.size(),
        paramset_size_ > 0 ? paramset_size_ : 1000,
        &rows, string_size, write_row, entity_at);
    } else {
      std::vector<std::vector<sql_value>> extracted;
      extracted.reserve(rows.size());
      for (auto const& row : rows) {
        std::vector<sql_value> vals;
        vals.reserve(m.columns.size());
        for (auto const& col : m.columns) {
          vals.push_back(col.read(&row));
        }
        extracted.push_back(std::move(vals));
      }
      result = insert_rowwise_impl(native_connection(), m, extracted, paramset_size_);
    }
    if (txn) txn->commit();
    return result;
  }

  // ========================================================================
  // READ (Query)
  // ========================================================================

  // --- Entity query entry point ---
  query_gateway query();

  // Typed aggregate projection query
  template <detail::aggregate_projection T>
  std::vector<T> query(std::string_view sql, params const& p = {}) {
    ensure_connected();
    std::string key(sql);
    auto& c = native_connection();
    auto stmt = c.acquire_statement(key);
    stmt->bind_params(p);
    stmt->execute();
    stmt->set_row_array_size(row_array_size_);
    detail::projection<T> proj;
    proj.set_row_array_size(row_array_size_);
    proj.bind(*stmt);
    std::vector<T> out;
    while (stmt->fetch()) {
      std::size_t rows_fetched = stmt->rows_fetched();
      for (std::size_t i = 0; i < rows_fetched; ++i) {
        out.push_back(proj.take(i));
      }
    }
    c.release_statement(key, std::move(stmt));
    return out;
  }

  // ========================================================================
  // UPDATE
  // ========================================================================

  // Dynamic update builder for tables without entity mapping
  update_builder update(std::string_view table);

  // Single entity update (uses primary key)
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

  // Single entity update with explicit where fields
  template <class Entity>
    requires(!std::is_convertible_v<Entity const&, std::string_view>)
  std::size_t update(
    Entity const& entity, std::vector<std::string> const& where_fields) {
    ensure_connected();
    entity_meta const& m = meta<Entity>();
    std::set<std::string> where_set(where_fields.begin(), where_fields.end());
    std::vector<std::string> set_columns;
    std::vector<sql_value> set_values;
    std::vector<sql_value> where_values;
    for (auto const& c : m.columns) {
      if (where_set.count(c.column)) {
        where_values.push_back(c.read(&entity));
      } else {
        set_columns.push_back(c.column);
        set_values.push_back(c.read(&entity));
      }
    }
    return update_single_impl(
      native_connection(), m, set_columns, set_values,
      where_fields, where_values);
  }

  // Batch entity update (uses primary key)
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

  // Batch entity update with explicit where fields
  template <class Entity>
  std::size_t update(std::vector<Entity> const& entities,
    std::vector<std::string> const& where_fields) {
    ensure_connected();
    if (entities.empty()) {
      return 0;
    }
    entity_meta const& m = meta<Entity>();
    std::set<std::string> where_set(where_fields.begin(), where_fields.end());
    std::vector<std::string> set_columns;
    std::vector<std::size_t> set_col_indices;
    for (std::size_t ci = 0; ci < m.columns.size(); ++ci) {
      if (!where_set.count(m.columns[ci].column)) {
        set_columns.push_back(m.columns[ci].column);
        set_col_indices.push_back(ci);
      }
    }
    std::vector<std::size_t> where_col_indices;
    for (auto const& f : where_fields) {
      for (std::size_t ci = 0; ci < m.columns.size(); ++ci) {
        if (m.columns[ci].column == f) {
          where_col_indices.push_back(ci);
          break;
        }
      }
    }

    if (native_connection().caps().columnar_batch) {
      auto string_size = +[](void const* meta, void const* entity,
                          std::size_t col) -> std::size_t {
        return static_cast<entity_meta const*>(meta)
          ->columns[col]
          .get_string_size(entity);
      };
      auto write_row = +[](void const* meta, void const* entity,
                        std::size_t row, std::size_t col, void* buf,
                        std::size_t stride, std::int64_t* inds) {
        static_cast<entity_meta const*>(meta)
          ->columns[col]
          .write_to_param_buffer(entity, row, buf, stride, inds);
      };
      auto entity_at = +[](void const* data,
                        std::size_t i) -> void const* {
        return &(*static_cast<std::vector<Entity> const*>(data))[i];
      };
      std::optional<transaction> txn;
      if (auto_commit_) txn.emplace(native_connection().begin());
      auto result = update_columnar_impl(native_connection(), m,
        set_columns, set_col_indices,
        where_fields, where_col_indices,
        entities.size(), paramset_size_ > 0 ? paramset_size_ : 1000,
        &entities, string_size, write_row, entity_at);
      if (txn) txn->commit();
      return result;
    }

    std::vector<std::vector<sql_value>> rows;
    rows.reserve(entities.size());
    for (auto const& e : entities) {
      std::vector<sql_value> rv;
      rv.reserve(set_columns.size() + where_fields.size());
      for (auto ci : set_col_indices) {
        rv.push_back(m.columns[ci].read(&e));
      }
      for (auto ci : where_col_indices) {
        rv.push_back(m.columns[ci].read(&e));
      }
      rows.push_back(std::move(rv));
    }
    std::optional<transaction> txn;
    if (auto_commit_) txn.emplace(native_connection().begin());
    auto result = update_batch_impl(
      native_connection(), m, set_columns, where_fields, rows, paramset_size_);
    if (txn) txn->commit();
    return result;
  }

  // ========================================================================
  // DELETE (Remove)
  // ========================================================================

  // Dynamic remove builder for tables without entity mapping
  remove_builder remove(std::string_view table);

  // ========================================================================
  // RAW SQL OPERATIONS
  // ========================================================================

  result_set execute(std::string_view sql, params const& p = {});
  std::size_t execute_update(std::string_view sql, params const& p = {});

  // --- Transaction ---
  transaction begin();
  void commit();
  void rollback();

  // --- Auto-commit control ---
  // When enabled (default), batch CRUD operations (insert/update of
  // entity collections) are automatically wrapped in a transaction.
  // Disable for manual transaction control via begin()/commit()/rollback().
  bool auto_commit() const noexcept { return auto_commit_; }
  void auto_commit(bool enabled) noexcept { auto_commit_ = enabled; }

  // --- Cache observability ---
  unsigned long long statement_cache_hits() const;
  unsigned long long statement_cache_misses() const;
  std::size_t statement_cache_size() const;
  void clear_statement_cache();

  // --- Escape hatch ---
  // Access the underlying connection for low-level operations.
  // Use this when you need backend-specific features like native handles
  // or extensions that are not exposed through the high-level orm API.
  connection& native_connection() {
    ensure_connected();
    return pooled_conn_->get();
  }

private:
  friend class query_gateway;
  friend class update_builder;
  friend class remove_builder;

  std::optional<pooled_connection> pooled_conn_;
  std::unordered_map<std::type_index, entity_meta> entities_;
  std::size_t row_array_size_ = 100;  // Default block fetch size
  std::size_t paramset_size_ = 1000;  // Default batch insert size
  bool auto_commit_ = true;           // Auto-wrap batch ops in transaction

  void ensure_connected() const;

  static std::size_t update_single_impl(connection& conn,
    entity_meta const& m,
    std::vector<std::string> const& set_columns,
    std::vector<sql_value> const& set_values,
    std::vector<std::string> const& where_fields,
    std::vector<sql_value> const& where_values);

  static std::size_t update_batch_impl(connection& conn,
    entity_meta const& m,
    std::vector<std::string> const& set_columns,
    std::vector<std::string> const& where_fields,
    std::vector<std::vector<sql_value>> const& rows,
    std::size_t default_batch_size);

  static std::size_t insert_rowwise_impl(connection& conn,
    entity_meta const& m,
    std::vector<std::vector<sql_value>> const& rows,
    std::size_t default_batch_size);

  using string_size_fn = std::size_t (*)(void const* meta,
    void const* entity, std::size_t col);
  using write_row_fn = void (*)(void const* meta, void const* entity,
    std::size_t row, std::size_t col,
    void* buf, std::size_t stride, std::int64_t* inds);

  static std::size_t insert_columnar_impl(connection& conn,
    entity_meta const& m, std::size_t row_count,
    std::size_t batch_size, void const* rows_data,
    string_size_fn string_size, write_row_fn write_row,
    void const* (*entity_at)(void const* data, std::size_t i));

  static std::size_t update_columnar_impl(connection& conn,
    entity_meta const& m,
    std::vector<std::string> const& set_columns,
    std::vector<std::size_t> const& set_col_indices,
    std::vector<std::string> const& where_fields,
    std::vector<std::size_t> const& where_col_indices,
    std::size_t row_count, std::size_t batch_size,
    void const* rows_data,
    string_size_fn string_size, write_row_fn write_row,
    void const* (*entity_at)(void const* data, std::size_t i));
};

}  // namespace uniorm
