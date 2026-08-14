#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <uniorm/detail/pfr.hpp>
#include <uniorm/detail/projection.hpp>
#include <uniorm/detail/statement_cache.hpp>
#include <uniorm/export.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/odbc/connection.hpp>
#include <uniorm/params.hpp>
#include <uniorm/result_set.hpp>

namespace uniorm {

class query_gateway;
class transaction;
template <class T>
class query;

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
    std::string key(sql);
    odbc::statement stmt = acquire_cached(key);
    auto staging = p.bind(stmt);
    stmt.execute();
    detail::projection<T> proj;
    proj.bind(stmt);
    std::vector<T> out;
    while (stmt.fetch()) {
      out.push_back(proj.take());
    }
    stmt_cache_->release(key, std::move(stmt));
    return out;
  }

  // Entity query entry point: conn.query(registry).of<T>()...
  query_gateway query(orm& registry);

  transaction begin();

  // Database product name reported by the driver (SQL_DBMS_NAME).
  std::string dbms_name() const;

  // Prepared-statement cache observability (keyed by SQL text, LRU).
  unsigned long long statement_cache_hits() const;
  unsigned long long statement_cache_misses() const;
  std::size_t statement_cache_size() const;
  void clear_statement_cache();

private:
  friend class orm;
  friend class transaction;
  template <class T>
  friend class query;

  // Prepare, bind parameters, execute, then hand the live statement to fn
  // so entity queries can SQLBindCol directly onto entity fields.
  template <class Fn>
  auto execute_with(std::string_view sql, params const& p, Fn&& fn) {
    std::string key(sql);
    odbc::statement stmt = acquire_cached(key);
    auto staging = p.bind(stmt);
    stmt.execute();
    auto result = fn(stmt);
    stmt_cache_->release(key, std::move(stmt));
    return result;
  }

  odbc::statement acquire_cached(std::string const& key) {
    return stmt_cache_->acquire(key, [this](std::string const& s) {
      odbc::statement stmt(conn_);
      stmt.prepare(s);
      return stmt;
    });
  }

  std::function<void(odbc::statement)> make_releaser(std::string key);

  void set_autocommit(bool enabled);
  void commit_txn();
  void rollback_txn();
  odbc::connection& odbc_conn() noexcept {
    return conn_;
  }

  odbc::connection conn_;
  // Shared so result_set check-in closures can hold weak references that
  // survive even if the connection is moved; destroyed with the connection
  // (declared after conn_, so cached statements are freed first).
  std::shared_ptr<detail::statement_cache> stmt_cache_;
};

}  // namespace uniorm
