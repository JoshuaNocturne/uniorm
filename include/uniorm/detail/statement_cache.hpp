#pragma once

// Per-connection LRU cache of prepared statements keyed by SQL text.
// Statements are checked out for use and returned when the consumer is
// done, so the cache only ever holds idle handles. Not thread-safe; the
// owning connection is not thread-safe either.

#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <uniorm/backend/backend.hpp>

namespace uniorm::detail {

struct statement_cache {
  std::unordered_map<std::string,
    std::unique_ptr<backend::statement_iface>>
    entries;
  std::list<std::string> lru;  // front = most recently returned
  std::size_t capacity = 64;
  unsigned long long hits = 0;
  unsigned long long misses = 0;

  // Returns a prepared statement for sql: a cached one (reset for reuse)
  // when available, otherwise a fresh statement from prepare(sql).
  template <class Prepare>
  std::unique_ptr<backend::statement_iface> acquire(
    std::string const& sql, Prepare&& prepare) {
    auto it = entries.find(sql);
    if (it != entries.end()) {
      ++hits;
      auto stmt = std::move(it->second);
      entries.erase(it);
      lru.remove(sql);
      stmt->reset();
      return stmt;
    }
    ++misses;
    return prepare(sql);
  }

  // Returns a used statement to the cache. Dropped when sql already has
  // a cached entry (a concurrent checkout produced this one) or the
  // cache is full; the least recently used entry is evicted to make room.
  void release(std::string const& sql,
    std::unique_ptr<backend::statement_iface> stmt) {
    if (entries.count(sql) != 0) {
      return;
    }
    stmt->reset();
    if (entries.size() >= capacity) {
      entries.erase(lru.back());
      lru.pop_back();
    }
    entries.emplace(sql, std::move(stmt));
    lru.push_front(sql);
  }
};

}  // namespace uniorm::detail
