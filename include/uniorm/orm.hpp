#pragma once

// Central entry point for uniorm: owns a database connection and entity
// mappings, provides all database operations.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/connection.hpp>
#include <uniorm/detail/projection.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/pool.hpp>
#include <uniorm/schema.hpp>
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
  // Rows materialised per fetch. 0 is clamped: the dynamic-row layer divides
  // buffer sizes by it to locate each row's slot.
  static constexpr std::size_t default_row_array_size = 100;

  std::size_t row_array_size() const noexcept;
  void row_array_size(std::size_t size) noexcept;

  // --- Batch operation configuration (insert/update) ---
  // Rows bound per execute(). 0 is clamped: the columnar loop advances by it.
  static constexpr std::size_t default_paramset_size = 1000;

  std::size_t paramset_size() const noexcept;
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

  entity_meta const* find(std::type_index type) const;

  std::size_t size() const noexcept;

  // Explicit, per-name resolution of every registered mapping against the
  // catalog at (catalog, schema). `validate()` reads the catalog but never
  // resolves; a mapping without a preceding call keeps the connection's
  // spelling policy. Re-running overwrites, and clear restores the policy.
  void resolve_identifiers(
    std::string_view catalog, std::string_view schema);
  void clear_identifier_resolution() noexcept;

  // Resolved names take precedence over the connection's spelling policy.
  void validate(validation_mode mode = validation_mode::strict);

  // ========================================================================
  // CREATE (Insert)
  // ========================================================================

  // The Entity type is needed only here; src/orm.cpp sees a byte stride.
  template <class Entity>
  std::size_t insert(std::vector<Entity> const& rows) {
    ensure_connected();
    return insert_impl(native_connection(), meta<Entity>(), rows.data(),
      sizeof(Entity), rows.size(), paramset_size_);
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
    if (std::size_t est = stmt->result_row_estimate()) out.reserve(est);
    while (stmt->fetch()) {
      std::size_t rows_fetched = stmt->rows_fetched();
      // Construct rows directly in the vector storage; see fill_into.
      std::size_t old_size = out.size();
      out.resize(out.size() + rows_fetched);
      try {
        for (std::size_t i = 0; i < rows_fetched; ++i) {
          proj.fill_into(out[old_size + i], i);
        }
      } catch (...) {
        out.resize(old_size);
        throw;
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
    return update_single_impl(
      native_connection(), meta<Entity>(), &entity, std::nullopt);
  }

  // Single entity update with explicit where fields
  template <class Entity>
    requires(!std::is_convertible_v<Entity const&, std::string_view>)
  std::size_t update(
    Entity const& entity, std::vector<std::string> const& where_fields) {
    ensure_connected();
    return update_single_impl(
      native_connection(), meta<Entity>(), &entity, where_fields);
  }

  // Batch entity update (uses primary key)
  template <class Entity>
  std::size_t update(std::vector<Entity> const& entities) {
    ensure_connected();
    return update_batch_impl(native_connection(), meta<Entity>(),
      entities.data(), sizeof(Entity), entities.size(), std::nullopt,
      paramset_size_);
  }

  // Batch entity update with explicit where fields
  template <class Entity>
  std::size_t update(std::vector<Entity> const& entities,
    std::vector<std::string> const& where_fields) {
    ensure_connected();
    return update_batch_impl(native_connection(), meta<Entity>(),
      entities.data(), sizeof(Entity), entities.size(), where_fields,
      paramset_size_);
  }

  // ========================================================================
  // DELETE (Remove)
  // ========================================================================

  // Dynamic remove builder for tables without entity mapping
  remove_builder remove(std::string_view table);

  // Single entity remove (uses primary key)
  template <class Entity>
    requires(!std::is_convertible_v<Entity const&, std::string_view>)
  std::size_t remove(Entity const& entity) {
    ensure_connected();
    return delete_single_impl(
      native_connection(), meta<Entity>(), &entity, std::nullopt);
  }

  // Single entity remove with explicit where fields
  template <class Entity>
    requires(!std::is_convertible_v<Entity const&, std::string_view>)
  std::size_t remove(Entity const& entity,
    std::vector<std::string> const& where_fields) {
    ensure_connected();
    return delete_single_impl(
      native_connection(), meta<Entity>(), &entity, where_fields);
  }

  // Batch entity remove (uses primary key)
  template <class Entity>
  std::size_t remove(std::vector<Entity> const& entities) {
    ensure_connected();
    return delete_batch_impl(native_connection(), meta<Entity>(),
      entities.data(), sizeof(Entity), entities.size(), std::nullopt,
      paramset_size_);
  }

  // Batch entity remove with explicit where fields
  template <class Entity>
  std::size_t remove(std::vector<Entity> const& entities,
    std::vector<std::string> const& where_fields) {
    ensure_connected();
    return delete_batch_impl(native_connection(), meta<Entity>(),
      entities.data(), sizeof(Entity), entities.size(), where_fields,
      paramset_size_);
  }

  // ========================================================================
  // RAW SQL OPERATIONS
  // ========================================================================

  result_set execute(std::string_view sql, params const& p = {});
  std::size_t execute_update(std::string_view sql, params const& p = {});

  // --- Transaction ---
  transaction begin();
  void commit();
  void rollback();

  // --- Commit mode ---
  // The connection's own autocommit attribute, so it governs every write shape
  // alike: on, each statement is durable as it stands (a batch sweep runs
  // inside a transaction so it cannot half-commit); off, nothing is durable
  // until commit(). Switching the mode on commits what is pending. A scope
  // opened with begin() overrides it while open and restores it when closed.
  bool auto_commit() const noexcept;
  void auto_commit(bool enabled);

  // --- Identifier spelling ---
  // Re-applied on every lease; resolved mappings bypass this policy.
  dialect::identifier_case identifier_case() const noexcept;
  void identifier_case(dialect::identifier_case policy);

  // --- Cache observability ---
  unsigned long long statement_cache_hits() const;
  unsigned long long statement_cache_misses() const;
  std::size_t statement_cache_size() const;
  void clear_statement_cache();

  // --- Table introspection ---
  // The live catalog behind this connection. Throws
  // backend::capability_not_supported when the backend offers no reads.
  schema_meta& schema();

  // --- Escape hatch ---
  // The underlying connection: for the native handle, and for the
  // dbms_name() / caps() the orm API does not forward.
  connection& native_connection();

private:
  friend class query_gateway;
  friend class update_builder;
  friend class remove_builder;

  std::optional<pooled_connection> pooled_conn_;
  std::unordered_map<std::type_index, entity_meta> entities_;
  std::size_t row_array_size_ = default_row_array_size;
  std::size_t paramset_size_ = default_paramset_size;
  bool auto_commit_ = true;  // commit mode applied to every connection
  // Spelling policy applied to every connection, as auto_commit_ is.
  dialect::identifier_case identifiers_ = dialect::identifier_case::keep;

  void ensure_connected() const;

  // Entity write entry points; everything below the surface of the public
  // templates lives in src/orm.cpp. `where_fields` == nullopt means "match on
  // every primary key column", row storage is (data, element stride, row
  // count), and `batch_size` must be non-zero.
  static std::size_t insert_impl(connection& conn, entity_meta const& m,
    void const* rows, std::size_t row_stride, std::size_t row_count,
    std::size_t batch_size);

  static std::size_t update_single_impl(connection& conn, entity_meta const& m,
    void const* entity,
    std::optional<std::vector<std::string>> const& where_fields);

  static std::size_t update_batch_impl(connection& conn, entity_meta const& m,
    void const* rows, std::size_t row_stride, std::size_t row_count,
    std::optional<std::vector<std::string>> const& where_fields,
    std::size_t batch_size);

  static std::size_t delete_single_impl(connection& conn, entity_meta const& m,
    void const* entity,
    std::optional<std::vector<std::string>> const& where_fields);

  static std::size_t delete_batch_impl(connection& conn, entity_meta const& m,
    void const* rows, std::size_t row_stride, std::size_t row_count,
    std::optional<std::vector<std::string>> const& where_fields,
    std::size_t batch_size);
};

}  // namespace uniorm
