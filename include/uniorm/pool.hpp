#pragma once

// Minimal fixed-size connection pool: lazy creation, borrow/return, timeout.
// All pools share ONE background scheduler thread (enabled per pool when
// heartbeat_interval > 0) which periodically runs heartbeat_sql on idle
// connections and drops any whose idle time exceeds max_idle_time or whose
// heartbeat fails. Connections idle beyond max_idle_time are also dropped
// when handed out by acquire(). The pool must outlive all connections still
// checked out from it.

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include <uniorm/connection.hpp>
#include <uniorm/error.hpp>
#include <uniorm/export.hpp>

namespace uniorm {

struct pool_options {
  std::string connection_string;
  std::size_t size = 8;
  std::chrono::milliseconds acquire_timeout{ 5000 };
  // 0 disables the background maintainer; acquire() still drops connections
  // idle beyond max_idle_time.
  std::chrono::milliseconds heartbeat_interval{ 30000 };
  std::chrono::milliseconds max_idle_time{ 600000 };
  std::string heartbeat_sql = "SELECT 1";
};

class connection_pool;

class UNIORM_API pooled_connection {
public:
  ~pooled_connection();

  pooled_connection(pooled_connection&&) noexcept;
  pooled_connection& operator=(pooled_connection&&) noexcept;

  pooled_connection(pooled_connection const&) = delete;
  pooled_connection& operator=(pooled_connection const&) = delete;

  connection& get() noexcept {
    return conn_;
  }
  connection* operator->() noexcept {
    return &conn_;
  }
  explicit operator bool() const noexcept {
    return valid_;
  }

private:
  friend class connection_pool;
  pooled_connection(connection_pool* pool, connection conn);

  connection_pool* pool_ = nullptr;
  connection conn_;
  bool valid_ = false;
};

namespace pool_detail {
struct shared_state;  // pool internals, shared with the global scheduler
}  // namespace pool_detail

class UNIORM_API connection_pool {
public:
  explicit connection_pool(pool_options options);
  ~connection_pool();

  connection_pool(connection_pool&&) = delete;
  connection_pool& operator=(connection_pool&&) = delete;
  connection_pool(connection_pool const&) = delete;
  connection_pool& operator=(connection_pool const&) = delete;

  pooled_connection acquire();  // throws pool_timeout after acquire_timeout
  std::size_t capacity() const;
  std::size_t idle_count() const;
  // Successful heartbeat statements executed by the maintainer so far.
  unsigned long long heartbeats_executed() const;

private:
  friend class pooled_connection;
  void release(connection conn);

  std::shared_ptr<pool_detail::shared_state>
    impl_;  // scheduler holds weak refs
};

}  // namespace uniorm
