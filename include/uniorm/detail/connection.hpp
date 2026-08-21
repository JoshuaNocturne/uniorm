#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/detail/pfr.hpp>
#include <uniorm/detail/projection.hpp>
#include <uniorm/detail/statement_cache.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/export.hpp>
#include <uniorm/params.hpp>
#include <uniorm/result_set.hpp>

#include <set>

namespace uniorm {

class orm;
class update_builder;
class remove_builder;
class query_gateway;
class transaction;
template <class T>
class query;

// High-level connection: owns a backend connection (chosen by the
// connection-string scheme at construction) and offers statement
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

  // Dynamic DELETE without an entity mapping. Column and table
  remove_builder remove(std::string_view table);

  // Dynamic UPDATE without an entity mapping.
  update_builder update(std::string_view table);

  template <detail::aggregate_projection T>
  std::vector<T> query(std::string_view sql, params const& p = {}) {
    std::string key(sql);
    auto stmt = acquire_cached(key);
    bind_parameters(*stmt, p);
    stmt->execute();
    detail::projection<T> proj;
    proj.bind(*stmt);
    std::vector<T> out;
    while (stmt->fetch()) {
      out.push_back(proj.take());
    }
    stmt_cache_->release(key, std::move(stmt));
    return out;
  }

  transaction begin();

  // Database product name reported by the backend.
  std::string dbms_name() const;

  // Prepared-statement cache observability (keyed by SQL text, LRU).
  unsigned long long statement_cache_hits() const;
  unsigned long long statement_cache_misses() const;
  std::size_t statement_cache_size() const;
  void clear_statement_cache();

  // Escape hatches (design doc 5.3): the caller names the expected
  // native handle type (void for ODBC's SQLHDBC, PGconn for libpq, ...)
  // knowing which backend it connected to; uniorm keeps owning the
  // connection and transaction lifecycle.
  template <class T>
  T* native_handle() noexcept {
    return backend_ ? static_cast<T*>(backend_->native_handle()) : nullptr;
  }

  // Typed backend extension (e.g. backend::schema_metadata); nullptr
  // when the backend does not offer it.
  template <class T>
  T* extension() noexcept {
    return backend_
             ? static_cast<T*>(backend_->extension(std::type_index(typeid(T))))
             : nullptr;
  }

private:
  friend class orm;
  friend class transaction;
  template <class T>
  friend class query;

  static void bind_parameters(backend::statement_iface& stmt, params const& p) {
    for (std::size_t i = 0; i < p.size(); ++i) {
      stmt.bind_parameter(i + 1, p.at(i));
    }
  }

  // Prepare, bind parameters, execute, then hand the live statement to fn
  // so entity queries can bind result columns directly onto entity fields.
  template <class Fn>
  auto execute_with(std::string_view sql, params const& p, Fn&& fn) {
    std::string key(sql);
    auto stmt = acquire_cached(key);
    bind_parameters(*stmt, p);
    stmt->execute();
    auto result = fn(*stmt);
    stmt_cache_->release(key, std::move(stmt));
    return result;
  }

  std::unique_ptr<backend::statement_iface> acquire_cached(
    std::string const& key) {
    return stmt_cache_->acquire(key, [this](std::string const& s) {
      auto stmt = backend_->create_statement();
      stmt->prepare(s);
      return stmt;
    });
  }

  std::function<void(std::unique_ptr<backend::statement_iface>)> make_releaser(
    std::string key);

  void set_autocommit(bool enabled);
  void commit();
  void rollback();

  // Declared before stmt_cache_ so cached statements are destroyed first.
  std::unique_ptr<backend::connection_iface> backend_;
  // Shared so result_set check-in closures can hold weak references that
  // survive even if the connection is moved.
  std::shared_ptr<detail::statement_cache> stmt_cache_;
};

}  // namespace uniorm
