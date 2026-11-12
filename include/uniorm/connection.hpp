#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/connection_spec.hpp>
#include <uniorm/detail/pfr.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/export.hpp>
#include <uniorm/params.hpp>
#include <uniorm/result_set.hpp>
#include <uniorm/schema.hpp>

namespace uniorm {

class transaction;
namespace detail {
// Defined in src/statement_cache.hpp (private header); the
// member below is only ever manipulated in connection.cpp.
struct statement_cache;
}  // namespace detail

// Low-level connection: owns a backend connection (the backend is the
// connection string's scheme, or a spec's, picked at construction) and
// offers statement execution, statement caching, and transaction control.
// orm is the high-level entry point; connection provides the primitives.
class UNIORM_API connection {
public:
  explicit connection(std::string_view connection_string);
  // The pieces reach the backend as they were given: only the scheme is
  // read above this line, and a backend that wants no part of them says so.
  explicit connection(connection_spec const& spec);
  ~connection();

  connection(connection&&) noexcept;
  connection& operator=(connection&&) noexcept;

  connection(connection const&) = delete;
  connection& operator=(connection const&) = delete;

  void close();
  bool is_open() const noexcept;

  // --- Simple execution ---
  result_set execute(
    std::string_view sql, params const& p = {}, std::size_t row_array_size = 1);
  std::size_t execute_update(std::string_view sql, params const& p = {});

  // --- Transaction control ---
  transaction begin();
  // The mode set_autocommit last installed, not "a transaction is open".
  bool autocommit() const noexcept;
  // Off: nothing is durable until commit()/rollback(). On: commits whatever is
  // pending, per the ODBC contract for this attribute.
  void set_autocommit(bool enabled);
  void commit();
  void rollback();

  // --- Statement cache primitives ---
  // Acquire a prepared statement for the given SQL (from cache or newly
  // prepared). The caller must return it via release_statement().
  std::unique_ptr<backend::statement_iface> acquire_statement(
    std::string const& sql);

  // Return a statement to the cache under the given key.
  void release_statement(
    std::string const& key, std::unique_ptr<backend::statement_iface> stmt);

  // --- Metadata ---
  std::string dbms_name() const;
  backend::capabilities caps() const noexcept;

  // --- Identifier spelling ---
  // How the SQL this connection generates quotes its names. Kept on the
  // connection because that is where the server's other quirks are known.
  // No value means the dialect detected from the server's name answers it.
  std::optional<dialect::identifier_case> identifier_case() const noexcept;
  void identifier_case(std::optional<dialect::identifier_case> policy);

  // The dialect built from the server's name and the policy above, made on
  // first use.
  dialect const& sql_dialect();

  // Prepared-statement cache observability (keyed by SQL text, LRU).
  unsigned long long statement_cache_hits() const;
  unsigned long long statement_cache_misses() const;
  std::size_t statement_cache_size() const;
  void clear_statement_cache();

  // Table introspection. Throws backend::capability_not_supported when the
  // backend offers none; the reference stays valid while this connection
  // does.
  schema_meta& schema() const;

  // The server's own spelling of each named column's type, in the order the
  // columns were asked for, and empty where the catalog has no answer. A
  // table's spelling is read once and kept for as long as the connection
  // lives, since the server does not change what a column is under it.
  // Asking costs nothing when the answer is already here.
  std::vector<std::string> column_type_names(
    schema_meta::table_ref const& table,
    std::vector<std::string> const& columns);

  // Escape hatch: the caller names the expected native handle type
  // (void* for ODBC's SQLHDBC, PGconn for libpq, ...) knowing which
  // backend it connected to.
  template <class T>
  T* native_handle() noexcept {
    return backend_ ? static_cast<T*>(backend_->native_handle()) : nullptr;
  }

private:
  std::function<void(std::unique_ptr<backend::statement_iface>)> make_releaser(
    std::string key);

  // Declared before stmt_cache_ so cached statements are destroyed first.
  std::unique_ptr<backend::connection_iface> backend_;
  // Shared so result_set check-in closures can hold weak references that
  // survive even if the connection is moved.
  std::shared_ptr<detail::statement_cache> stmt_cache_;
  // Mirrors the last mode the backend was told; ODBC connects autocommitting.
  bool autocommit_ = true;
  // A declared spelling policy; empty defers to the server's own dialect.
  std::optional<dialect::identifier_case> identifiers_;
  // The fold that dialect answered, kept so clearing a policy needs no second
  // question to the server.
  dialect::identifier_case dialect_identifiers_ =
    dialect::identifier_case::keep;
  // Built on first spelling. A closed connection is never reopened, so the
  // server facts it holds cannot go stale; the policy is rewritten in place.
  std::optional<dialect> sql_dialect_;
  // What the catalog said each column's type is, per qualified table name,
  // read on first ask. It lives here for the same reason the statement cache
  // does: a server does not change an answer while the connection stands.
  std::map<std::string, std::vector<std::pair<std::string, std::string>>>
    column_types_;
};

}  // namespace uniorm
