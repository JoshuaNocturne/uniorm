#include <uniorm/connection.hpp>

#include <algorithm>
#include <cctype>
#include <memory>

#include "statement_cache.hpp"
#include "uniorm/backend/error.hpp"
#include "uniorm/backend/registry.hpp"
#include "uniorm/builder/builder.hpp"
#include "uniorm/dialect.hpp"
#include "uniorm/transaction.hpp"

namespace uniorm {

connection::connection(std::string_view connection_string)
  : stmt_cache_(std::make_shared<detail::statement_cache>()) {
  std::string tail;
  backend_ = backend::registry::instance().create(connection_string, &tail);
  backend_->open(tail);
}

connection::connection(connection_spec const& spec)
  : stmt_cache_(std::make_shared<detail::statement_cache>()) {
  backend_ = backend::registry::instance().create_for_scheme(spec.scheme);
  backend_->open(spec);
}

connection::~connection() = default;

connection::connection(connection&&) noexcept = default;

connection& connection::operator=(connection&&) noexcept = default;

void connection::close() {
  clear_statement_cache();
  column_types_.clear();
  backend_->close();
}

bool connection::is_open() const noexcept {
  return backend_ && backend_->is_open();
}

result_set connection::execute(
  std::string_view sql, params const& p, std::size_t row_array_size) {
  std::string key(sql);
  auto stmt = acquire_statement(key);
  stmt->bind_params(p);
  stmt->execute();
  return result_set::from_statement(
    std::move(stmt), make_releaser(key), row_array_size);
}

std::size_t connection::execute_update(std::string_view sql, params const& p) {
  std::string key(sql);
  auto stmt = acquire_statement(key);
  stmt->bind_params(p);
  stmt->execute();
  std::size_t affected = stmt->affected_rows();
  release_statement(key, std::move(stmt));
  return affected;
}

std::unique_ptr<backend::statement_iface> connection::acquire_statement(
  std::string const& sql) {
  return stmt_cache_->acquire(sql, [this](std::string const& s) {
    auto stmt = backend_->create_statement();
    stmt->prepare(s);
    return stmt;
  });
}

void connection::release_statement(
  std::string const& key, std::unique_ptr<backend::statement_iface> stmt) {
  stmt_cache_->release(key, std::move(stmt));
}

std::function<void(std::unique_ptr<backend::statement_iface>)>
connection::make_releaser(std::string key) {
  std::weak_ptr<detail::statement_cache> weak = stmt_cache_;
  return [weak, key = std::move(key)](
           std::unique_ptr<backend::statement_iface> stmt) noexcept {
    if (auto cache = weak.lock()) {
      cache->release(key, std::move(stmt));
    }
  };
}

unsigned long long connection::statement_cache_hits() const {
  return stmt_cache_->hits;
}

unsigned long long connection::statement_cache_misses() const {
  return stmt_cache_->misses;
}

std::size_t connection::statement_cache_size() const {
  return stmt_cache_->entries.size();
}

void connection::clear_statement_cache() {
  stmt_cache_->entries.clear();
  stmt_cache_->lru.clear();
}

transaction connection::begin() {
  return transaction(*this);
}

bool connection::autocommit() const noexcept {
  return autocommit_;
}

std::string connection::dbms_name() const {
  return backend_->dbms_name();
}

std::optional<dialect::identifier_case>
connection::identifier_case() const noexcept {
  return identifiers_;
}

void connection::identifier_case(
  std::optional<dialect::identifier_case> policy) {
  identifiers_ = policy;
  if (sql_dialect_) {
    sql_dialect_->identifiers = policy.value_or(dialect_identifiers_);
  }
}

dialect const& connection::sql_dialect() {
  if (!sql_dialect_) {
    dialect d = dialect::detect(dbms_name());
    d.accept_server_version(backend_->server_version());
    dialect_identifiers_ = d.identifiers;
    if (identifiers_) {
      d.identifiers = *identifiers_;
    }
    sql_dialect_ = d;
  }
  return *sql_dialect_;
}

backend::capabilities connection::caps() const noexcept {
  return backend_->caps();
}

schema_meta& connection::schema() const {
  schema_meta* md = backend_->schema();
  if (md == nullptr) {
    throw backend::capability_not_supported(
      "this backend offers no schema introspection");
  }
  return *md;
}

namespace {

// The name a catalog read is filed under, so two spellings of the same
// namespace do not each keep an answer.
std::string qualified_table(schema_meta::table_ref const& table) {
  std::string name = table.catalog;
  if (!table.schema.empty()) {
    if (!name.empty()) {
      name += ".";
    }
    name += table.schema;
  }
  if (!table.name.empty()) {
    if (!name.empty()) {
      name += ".";
    }
    name += table.name;
  }
  return name;
}

// What the catalog says each of this table's columns is, pairs of the name the
// catalog gave and the type spelling it gave with it. A backend that offers no
// introspection is not an error to a caller that can build its statement
// another way, so that answer is an empty list.
std::vector<std::pair<std::string, std::string>> read_column_types(
  connection& conn, schema_meta::table_ref const& table) {
  std::vector<std::pair<std::string, std::string>> types;
  std::vector<schema_meta::column_row> columns;
  try {
    columns = conn.schema().table_columns(table);
  } catch (backend::capability_not_supported const&) {
    return types;
  }
  types.reserve(columns.size());
  for (auto& column : columns) {
    types.emplace_back(
      std::move(column.shape.name), std::move(column.type_name));
  }
  return types;
}

}  // namespace

std::vector<std::string> connection::column_type_names(
  schema_meta::table_ref const& table,
  std::vector<std::string> const& columns) {
  std::string const name = qualified_table(table);
  auto const filed = column_types_.find(name);
  auto const& stored =
    filed != column_types_.end()
      ? filed->second
      : column_types_.try_emplace(name, read_column_types(*this, table))
          .first->second;
  std::vector<std::string> spellings;
  spellings.reserve(columns.size());
  for (auto const& column : columns) {
    std::string spelling;
    for (auto const& [listed, type] : stored) {
      if (listed == column) {
        spelling = type;
        break;
      }
    }
    spellings.push_back(std::move(spelling));
  }
  return spellings;
}

void connection::set_autocommit(bool enabled) {
  backend_->set_autocommit(enabled);
  autocommit_ = enabled;
}

void connection::commit() {
  backend_->commit();
}

void connection::rollback() {
  backend_->rollback();
}

}  // namespace uniorm
