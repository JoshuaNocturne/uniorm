#include "uniorm/connection.hpp"

#include <algorithm>
#include <memory>

#include "uniorm/backend/registry.hpp"
#include "uniorm/dialect.hpp"
#include "uniorm/query/builder.hpp"
#include "uniorm/transaction.hpp"

namespace uniorm {

connection::connection(std::string_view connection_string)
  : stmt_cache_(std::make_shared<detail::statement_cache>()) {
  std::string tail;
  backend_ = backend::registry::instance().create(connection_string, &tail);
  backend_->open(tail);
}

connection::~connection() = default;

connection::connection(connection&&) noexcept = default;

connection& connection::operator=(connection&&) noexcept = default;

void connection::close() {
  clear_statement_cache();
  backend_->close();
}

bool connection::is_open() const noexcept {
  return backend_ && backend_->is_open();
}

result_set connection::execute(std::string_view sql, params const& p) {
  std::string key(sql);
  auto stmt = acquire_cached(key);
  bind_parameters(*stmt, p);
  stmt->execute();
  return result_set::from_statement(std::move(stmt), make_releaser(key));
}

std::size_t connection::execute_update(std::string_view sql, params const& p) {
  std::string key(sql);
  auto stmt = acquire_cached(key);
  bind_parameters(*stmt, p);
  stmt->execute();
  std::size_t affected = stmt->affected_rows();
  stmt_cache_->release(key, std::move(stmt));
  return affected;
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

namespace {

// Keeps multi-row VALUES statements well below driver placeholder limits
// (MySQL/MariaDB allow ~65535 per statement).
constexpr std::size_t max_placeholders_per_statement = 4096;

}  // namespace

std::size_t connection::insert_batch(std::string_view table,
  std::vector<std::string> const& columns, std::vector<params> const& rows) {
  if (rows.empty()) {
    return 0;
  }
  if (columns.empty()) {
    throw uniorm_error("insert_batch: no columns specified");
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].size() != columns.size()) {
      throw uniorm_error("insert_batch: row " + std::to_string(i) + " has " +
                         std::to_string(rows[i].size()) + " values, expected " +
                         std::to_string(columns.size()));
    }
  }

  std::size_t rows_per_batch = max_placeholders_per_statement / columns.size();
  if (rows_per_batch == 0) {
    throw uniorm_error("insert_batch: too many columns per row");
  }

  dialect const d = dialect::detect(dbms_name());
  std::string prefix = "INSERT INTO " + d.quote_identifier(table) + " (";
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i != 0) {
      prefix += ", ";
    }
    prefix += d.quote_identifier(columns[i]);
  }
  prefix += ") VALUES ";
  std::string tuple = "(?";
  for (std::size_t i = 1; i < columns.size(); ++i) {
    tuple += ", ?";
  }
  tuple += ")";

  std::size_t inserted = 0;
  transaction txn = begin();
  for (std::size_t start = 0; start < rows.size(); start += rows_per_batch) {
    std::size_t end = std::min(start + rows_per_batch, rows.size());
    std::string sql = prefix;
    std::vector<sql_value> values;
    values.reserve((end - start) * columns.size());
    for (std::size_t i = start; i < end; ++i) {
      if (i != start) {
        sql += ", ";
      }
      sql += tuple;
      auto const& row_values = rows[i].values();
      values.insert(values.end(), row_values.begin(), row_values.end());
    }
    inserted += execute_update(sql, params(std::move(values)));
  }
  txn.commit();
  return inserted;
}

query_gateway connection::query(orm& registry) {
  return query_gateway(*this, registry);
}

transaction connection::begin() {
  return transaction(*this);
}

std::string connection::dbms_name() const {
  return backend_->dbms_name();
}

void connection::set_autocommit(bool enabled) {
  backend_->set_autocommit(enabled);
}

void connection::commit_txn() {
  backend_->commit();
}

void connection::rollback_txn() {
  backend_->rollback();
}

}  // namespace uniorm
