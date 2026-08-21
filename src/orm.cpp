#include "uniorm/orm.hpp"

#include <unordered_map>
#include <utility>

#include "uniorm/backend/backend.hpp"
#include <uniorm/detail/connection.hpp>
#include "uniorm/query/builder.hpp"
#include "uniorm/result_set.hpp"
#include "uniorm/transaction.hpp"

namespace uniorm {

namespace {

struct schema_column {
  bool nullable = false;
};

std::unordered_map<std::string, schema_column> load_table_schema(
  backend::schema_metadata& md, std::string const& table) {
  std::unordered_map<std::string, schema_column> schema;
  for (auto const& c : md.table_columns(table)) {
    schema[c.name] = schema_column{c.nullable};
  }
  return schema;
}

}  // namespace

// --- Connection lifecycle ---

orm::orm(std::string_view connection_string)
  : pooled_conn_(
      connection_pool_registry::instance().acquire(std::string(connection_string))) {}

orm::orm(connection_pool& pool)
  : pooled_conn_(pool.acquire()) {}

orm::~orm() = default;

orm::orm(orm&&) noexcept = default;

orm& orm::operator=(orm&&) noexcept = default;

void orm::connect(std::string_view connection_string) {
  pooled_conn_ =
    connection_pool_registry::instance().acquire(std::string(connection_string));
}

void orm::disconnect() {
  ensure_connected();
  pooled_conn_.reset();  // returns connection to pool
}

void orm::ensure_connected() const {
  if (!pooled_conn_ || !pooled_conn_->get().is_open()) {
    throw uniorm_error("orm: not connected; call connect() first");
  }
}

// --- Validation ---

void orm::validate(validation_mode mode) {
  ensure_connected();
  auto* md = pooled_conn_->get().extension<backend::schema_metadata>();
  if (md == nullptr) {
    throw mapping_error(
      "schema validation requires a backend that exposes schema metadata");
  }
  for (auto const& [type, meta] : entities_) {
    auto schema = load_table_schema(*md, meta.table);
    if (schema.empty()) {
      throw mapping_error("table not found: " + meta.table);
    }
    for (auto const& c : meta.columns) {
      auto it = schema.find(c.column);
      if (it == schema.end()) {
        throw mapping_error(
          "column not found in table " + meta.table + ": " + c.column);
      }
      if (mode == validation_mode::strict && it->second.nullable &&
          !c.nullable) {
        throw mapping_error("column " + meta.table + "." + c.column +
                            " is nullable but the mapped member is not "
                            "std::optional");
      }
    }
  }
}

// --- Entity query entry point ---

query_gateway orm::query() {
  ensure_connected();
  return query_gateway(*this);
}

// --- Raw SQL operations ---

result_set orm::execute(std::string_view sql, params const& p) {
  ensure_connected();
  return pooled_conn_->get().execute(sql, p);
}

std::size_t orm::execute_update(std::string_view sql, params const& p) {
  ensure_connected();
  return pooled_conn_->get().execute_update(sql, p);
}

// --- Dynamic (non-entity) operations ---

std::size_t orm::insert_batch(std::string_view table,
  std::vector<std::string> const& columns, std::vector<params> const& rows) {
  ensure_connected();
  return pooled_conn_->get().insert_batch(table, columns, rows);
}

update_builder orm::update(std::string_view table) {
  ensure_connected();
  return pooled_conn_->get().update(table);
}

remove_builder orm::remove(std::string_view table) {
  ensure_connected();
  return pooled_conn_->get().remove(table);
}

// --- Transaction ---

transaction orm::begin() {
  ensure_connected();
  return pooled_conn_->get().begin();
}

void orm::commit() {
  ensure_connected();
  pooled_conn_->get().commit();
}

void orm::rollback() {
  ensure_connected();
  pooled_conn_->get().rollback();
}

// --- Cache observability ---

unsigned long long orm::statement_cache_hits() const {
  ensure_connected();
  return pooled_conn_->get().statement_cache_hits();
}

unsigned long long orm::statement_cache_misses() const {
  ensure_connected();
  return pooled_conn_->get().statement_cache_misses();
}

std::size_t orm::statement_cache_size() const {
  ensure_connected();
  return pooled_conn_->get().statement_cache_size();
}

void orm::clear_statement_cache() {
  ensure_connected();
  pooled_conn_->get().clear_statement_cache();
}

}  // namespace uniorm
