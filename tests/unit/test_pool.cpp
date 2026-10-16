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
  int spec_opens = 0;
  int string_opens = 0;
  std::string spec_target;

  void open(std::string_view) override {
    ++string_opens;
  }
  void open(uniorm::connection_spec const& spec) override {
    ++spec_opens;
    spec_target = spec.target;
  }
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
  opts.target = "poolfake://x";
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

void test_registry_leases_by_spec() {
  uniorm::connection_spec spec{ "poolfake", "pieces", "who", "first" };
  uniorm::pool_options opts = pool_options_for();
  opts.size = 1;
  uniorm::connection_pool_registry& registry =
    uniorm::connection_pool_registry::instance();
  registry.configure(spec, opts);

  {
    uniorm::pooled_connection held = registry.acquire(spec);
    CHECK(bool(held));
    CHECK(current->spec_opens == 1);
    CHECK(current->spec_target == "pieces");
  }
  // A password stays out of the key, so a spec differing only there reaches
  // the same pool and gets the connection back rather than a fresh open.
  uniorm::connection_spec same_place{ "poolfake", "pieces", "who", "second" };
  uniorm::pooled_connection again = registry.acquire(same_place);
  CHECK(bool(again));
  CHECK(current->spec_opens == 1);
}

void test_spec_pool_does_not_share_a_string_pool() {
  uniorm::connection_spec spec{ "poolfake", "split", "who", "" };
  std::string const spelled = "poolfake://DSN=split;UID=who";
  uniorm::pool_options opts = pool_options_for();
  opts.size = 1;
  uniorm::connection_pool_registry& registry =
    uniorm::connection_pool_registry::instance();
  registry.configure(spec, opts);
  registry.configure(spelled, opts);

  uniorm::pooled_connection by_spec = registry.acquire(spec);
  CHECK(bool(by_spec));
  // The same place reached by the same user, spelled as a string: still a
  // second pool, since deciding the two are one takes a driver's vocabulary.
  uniorm::pooled_connection by_string = registry.acquire(spelled);
  CHECK(bool(by_string));
  CHECK_THROWS(registry.acquire(spec), uniorm::pool_timeout);
}

void test_orm_leases_by_spec() {
  uniorm::connection_spec spec{ "poolfake", "ormpieces", "who", "" };
  uniorm::pool_options opts = pool_options_for();
  opts.size = 1;
  uniorm::connection_pool_registry::instance().configure(spec, opts);

  {
    uniorm::orm db(spec);
    db.auto_commit(false);
    CHECK(current->spec_opens == 1);
    CHECK(current->autocommit == false);
  }
  CHECK(current->rollback_calls == 1);
  CHECK(current->autocommit == true);
}

// Moving an orm that is already connected over to the pieces route: the lease
// is dropped first, since a size-1 pool cannot hand a second one out, and the
// connection that comes back is opened from the pieces rather than a string.
void test_orm_reconnects_by_spec() {
  uniorm::connection_spec spec{ "poolfake", "reconnect", "who", "secret" };
  std::string const spelled = "poolfake://x;DSN=reconnect;UID=who";
  uniorm::pool_options opts = pool_options_for();
  opts.size = 1;
  uniorm::connection_pool_registry& registry =
    uniorm::connection_pool_registry::instance();
  registry.configure(spelled, opts);
  registry.configure(spec, opts);

  uniorm::orm db(spelled);
  CHECK(current->string_opens == 1);
  CHECK(current->spec_opens == 0);
  db.disconnect();
  db.connect(spec);
  CHECK(current->string_opens == 0);
  CHECK(current->spec_opens == 1);
  CHECK(current->spec_target == "reconnect");
}

// A pool made from a spec keeps that recipe for the connections it has to make
// later, not just the first one.
void test_spec_pool_refills_from_the_spec() {
  uniorm::connection_spec spec{ "poolfake", "refill", "who", "secret" };
  uniorm::pool_options opts = pool_options_for();
  opts.target = spec;
  opts.size = 1;
  fail_next_rollback = true;
  {
    uniorm::connection_pool pool(opts);
    {
      uniorm::orm db(pool);
      CHECK(current->spec_opens == 1);
      CHECK(current->string_opens == 0);
      db.auto_commit(false);
    }
    // Retired on the way back, so the next borrow opens a fresh one from the
    // recipe the pool holds -- which for this pool is the pieces.
    uniorm::pooled_connection fresh = pool.acquire();
    CHECK(bool(fresh));
    CHECK(current->spec_opens == 1);
    CHECK(current->string_opens == 0);
    CHECK(current->spec_target == "refill");
  }
  fail_next_rollback = false;
}

}  // namespace

void test_pool() {
  register_fake_backend();
  test_release_drops_the_previous_lease();
  test_release_retires_an_unusable_connection();
  test_schema_reports_a_backend_without_introspection();
  test_registry_leases_by_spec();
  test_spec_pool_does_not_share_a_string_pool();
  test_orm_leases_by_spec();
  test_orm_reconnects_by_spec();
  test_spec_pool_refills_from_the_spec();
}
