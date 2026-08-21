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

#include <uniorm/detail/connection.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/pool.hpp>

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
    std::vector<std::string> columns;
    columns.reserve(m.columns.size());
    for (auto const& c : m.columns) {
      columns.push_back(c.column);
    }
    std::vector<params> values;
    values.reserve(rows.size());
    for (auto const& obj : rows) {
      std::vector<sql_value> row_values;
      row_values.reserve(m.columns.size());
      for (auto const& c : m.columns) {
        row_values.push_back(c.read(&obj));
      }
      values.emplace_back(std::move(row_values));
    }
    return insert_batch(m.table, columns, values);
  }

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

  void ensure_connected() const;
};

}  // namespace uniorm
