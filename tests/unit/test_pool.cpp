#include "check.hpp"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/error.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/pool.hpp>

namespace {

// Reports every transaction call the pool layer makes, so the state a lease
// leaves behind is observable without a driver in the picture.
class recording_connection : public uniorm::backend::connection_iface {
public:
  bool fail_rollback = false;
  int rollback_calls = 0;
  int commit_calls = 0;
  bool autocommit = true;

  void open(std::string_view) override {}
  void close() override {}
  bool is_open() const noexcept override {
    return true;
  }
  void set_autocommit(bool enabled) override {
    autocommit = enabled;
  }
  void commit() override {
    ++commit_calls;
  }
  void rollback() override {
    if (fail_rollback) {
      throw std::runtime_error("rollback failed");
    }
    ++rollback_calls;
  }
  uniorm::backend::capabilities caps() const noexcept override {
    return {};
  }
  std::string dbms_name() const override {
    return "recording";
  }
  std::unique_ptr<uniorm::backend::statement_iface>
  create_statement() override {
    return nullptr;
  }
  void* native_handle() noexcept override {
    return nullptr;
  }
};

recording_connection* current = nullptr;
bool fail_next_rollback = false;

uniorm::pool_options pool_options_for() {
  uniorm::pool_options opts;
  opts.connection_string = "poolfake://x";
  opts.size = 1;
  opts.acquire_timeout = std::chrono::milliseconds(200);
  opts.heartbeat_interval = std::chrono::milliseconds(0);  // no maintainer
  return opts;
}

void register_fake_backend() {
  uniorm::backend::registry::instance().register_backend(
    "poolfake", [] {
      auto conn = std::make_unique<recording_connection>();
      conn->fail_rollback = fail_next_rollback;
      current = conn.get();
      return conn;
    });
}

void test_release_drops_the_previous_lease() {
  {
    uniorm::connection_pool pool(pool_options_for());
    {
      uniorm::orm db(pool);
      db.auto_commit(false);
      CHECK(current->autocommit == false);
      CHECK(current->rollback_calls == 0);
    }
    // The lease is gone, but its manual-commit mode and its uncommitted work
    // must not be.
    CHECK(current->rollback_calls == 1);
    CHECK(current->autocommit == true);
    CHECK(current->commit_calls == 0);  // dropped, not committed
    CHECK(pool.idle_count() == 1);

    uniorm::pooled_connection again = pool.acquire();
    CHECK(current->autocommit == true);
  }
}

void test_release_retires_an_unusable_connection() {
  fail_next_rollback = true;
  {
    uniorm::connection_pool pool(pool_options_for());
    {
      uniorm::orm db(pool);
      db.auto_commit(false);
    }
    // Resetting it failed, so the pool keeps rather than hands on a connection
    // it cannot vouch for; the freed slot has to let the next borrow through.
    CHECK(pool.idle_count() == 0);
    uniorm::pooled_connection fresh = pool.acquire();
    CHECK(bool(fresh));
    CHECK(current->rollback_calls == 0);  // a brand new connection
  }
  fail_next_rollback = false;
}

// The facade's introspection is the connection's, so a backend answering none
// is reported as that rather than as a missing lease.
void test_schema_reports_a_backend_without_introspection() {
  uniorm::connection_pool pool(pool_options_for());
  uniorm::orm db(pool);
  CHECK_THROWS(db.schema(), uniorm::backend::capability_not_supported);
}

}  // namespace

void test_pool() {
  register_fake_backend();
  test_release_drops_the_previous_lease();
  test_release_retires_an_unusable_connection();
  test_schema_reports_a_backend_without_introspection();
}
