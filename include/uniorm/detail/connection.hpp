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
#include <uniorm/dialect.hpp>
#include <uniorm/export.hpp>
#include <uniorm/params.hpp>
#include <uniorm/result_set.hpp>

namespace uniorm {

class transaction;
namespace detail {
// Defined in src/statement_cache.hpp (private header); the
// member below is only ever manipulated in connection.cpp.
struct statement_cache;
}  // namespace detail

// Low-level connection: owns a backend connection (chosen by the
// connection-string scheme at construction) and offers statement
// execution, statement caching, and transaction control.
// orm is the high-level entry point; connection provides the primitives.
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

  // --- Simple execution ---
  result_set execute(std::string_view sql, params const& p = {},
    std::size_t row_array_size = 1);
  std::size_t execute_update(std::string_view sql, params const& p = {});

  // --- Transaction control ---
  transaction begin();
  void set_autocommit(bool enabled);
  void commit();
  void rollback();

  // --- Statement cache primitives ---
  // Acquire a prepared statement for the given SQL (from cache or newly
  // prepared). The caller must return it via release_statement().
  std::unique_ptr<backend::statement_iface> acquire_statement(
    std::string const& sql);

  // Return a statement to the cache under the given key.
  void release_statement(std::string const& key,
    std::unique_ptr<backend::statement_iface> stmt);

  // --- Metadata ---
  std::string dbms_name() const;
  backend::capabilities caps() const noexcept { return backend_->caps(); }

  // Prepared-statement cache observability (keyed by SQL text, LRU).
  unsigned long long statement_cache_hits() const;
  unsigned long long statement_cache_misses() const;
  std::size_t statement_cache_size() const;
  void clear_statement_cache();

  // Escape hatches: the caller names the expected native handle type
  // (void* for ODBC's SQLHDBC, PGconn for libpq, ...) knowing which
  // backend it connected to.
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
  std::function<void(std::unique_ptr<backend::statement_iface>)> make_releaser(
    std::string key);

  // Declared before stmt_cache_ so cached statements are destroyed first.
  std::unique_ptr<backend::connection_iface> backend_;
  // Shared so result_set check-in closures can hold weak references that
  // survive even if the connection is moved.
  std::shared_ptr<detail::statement_cache> stmt_cache_;
};

}  // namespace uniorm
