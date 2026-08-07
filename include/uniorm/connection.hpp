#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "detail/pfr.hpp"
#include "detail/projection.hpp"
#include "export.hpp"
#include "mapping/registry.hpp"
#include "odbc/connection.hpp"
#include "params.hpp"
#include "result_set.hpp"

namespace uniorm {

class query_gateway;
class transaction;

// High-level connection: owns an ODBC connection and offers statement
// execution plus typed aggregate projection queries.
class UNIORM_API connection {
public:
  explicit connection(std::string_view connection_string);
  ~connection();

  connection(connection&&) noexcept;
  connection& operator=(connection&&) noexcept;

  connection(connection const&) = delete;
  connection& operator=(connection const&) = delete;

  void close();
  bool is_open() const noexcept;

  result_set execute(std::string_view sql, params const& p = {});
  std::size_t execute_update(std::string_view sql, params const& p = {});

  // Dynamic batch insert: one multi-row VALUES statement per chunk, all
  // wrapped in a single transaction. Every row must carry exactly
  // columns.size() parameter values. Returns the number of rows inserted.
  std::size_t insert_batch(std::string_view table,
    std::vector<std::string> const& columns, std::vector<params> const& rows);

  // Entity batch insert driven by the registered mapping: all mapped
  // columns are written, std::optional fields without a value become NULL.
  template <class Entity>
  std::size_t insert(orm const& registry, std::vector<Entity> const& rows) {
    entity_meta const& meta = registry.meta<Entity>();
    std::vector<std::string> columns;
    columns.reserve(meta.columns.size());
    for (auto const& c : meta.columns) {
      columns.push_back(c.column);
    }
    std::vector<params> values;
    values.reserve(rows.size());
    for (auto const& obj : rows) {
      std::vector<sql_value> row_values;
      row_values.reserve(meta.columns.size());
      for (auto const& c : meta.columns) {
        row_values.push_back(c.read(&obj));
      }
      values.emplace_back(std::move(row_values));
    }
    return insert_batch(meta.table, columns, values);
  }

  template <detail::aggregate_projection T>
  std::vector<T> query(std::string_view sql, params const& p = {}) {
    odbc::statement stmt(conn_);
    stmt.prepare(sql);
    auto staging = p.bind(stmt);
    stmt.execute();
    detail::projection<T> proj;
    proj.bind(stmt);
    std::vector<T> out;
    while (stmt.fetch()) {
      out.push_back(proj.take());
    }
    return out;
  }

  // Entity query entry point: conn.query(registry).of<T>()...
  query_gateway query(orm& registry);

  transaction begin();

  // Database product name reported by the driver (SQL_DBMS_NAME).
  std::string dbms_name() const;

private:
  friend class orm;
  friend class transaction;

  void set_autocommit(bool enabled);
  void commit_txn();
  void rollback_txn();
  odbc::connection& odbc_conn() noexcept {
    return conn_;
  }

  odbc::connection conn_;
};

}  // namespace uniorm
