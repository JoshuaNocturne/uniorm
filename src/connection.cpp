#include <uniorm/connection.hpp>

#include <algorithm>
#include <cctype>
#include <memory>

#include "uniorm/backend/registry.hpp"
#include "uniorm/dialect.hpp"
#include "uniorm/builder/builder.hpp"
#include "statement_cache.hpp"
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

result_set connection::execute(std::string_view sql, params const& p,
  std::size_t row_array_size) {
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

void connection::release_statement(std::string const& key,
  std::unique_ptr<backend::statement_iface> stmt) {
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

backend::capabilities connection::caps() const noexcept {
  return backend_->caps();
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
