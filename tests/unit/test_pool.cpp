#include "check.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

// A full pool parks its waiter on that pool alone: the registry's lock is
// dropped before the wait, so a first lease of another key is not made to sit
// out someone else's acquire_timeout.
void test_registry_waiter_does_not_stall_other_keys() {
  uniorm::connection_pool_registry& registry =
    uniorm::connection_pool_registry::instance();
  std::string const full = "poolfake://x;DSN=stallfull;UID=who";
  std::string const open_key = "poolfake://x;DSN=stallother;UID=who";
  uniorm::pool_options tight = pool_options_for();
  tight.size = 1;
  tight.acquire_timeout = std::chrono::milliseconds(600);
  registry.configure(full, tight);

  uniorm::pooled_connection held = registry.acquire(full);
  CHECK(bool(held));
  bool waiter_timed_out = false;
  bool waiter_kept_a_lease = false;
  std::thread waiter([&] {
    try {
      uniorm::pooled_connection blocked = registry.acquire(full);
      waiter_kept_a_lease = bool(blocked);
    } catch (uniorm::pool_timeout const&) {
      waiter_timed_out = true;
    } catch (...) {
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let it park

  auto started = std::chrono::steady_clock::now();
  uniorm::pooled_connection elsewhere = registry.acquire(open_key);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started).count();
  CHECK(bool(elsewhere));
  CHECK(elapsed < 300);

  waiter.join();
  CHECK(waiter_timed_out);
  CHECK(!waiter_kept_a_lease);
}

// A heartbeat is a real round trip through the connection layer, so the
// maintainer path needs a statement that answers: prepare, execute, describe
// as no columns (nothing here fetches) and reset on the way back into the
// connection's cache.
struct beat_state {
  std::atomic<unsigned long long> executes{ 0 };
};

class beat_statement : public uniorm::backend::statement_iface {
public:
  explicit beat_statement(std::shared_ptr<beat_state> state)
    : state_(std::move(state)) {}

  void prepare(std::string_view) override {}
  void execute() override {
    state_->executes.fetch_add(1, std::memory_order_relaxed);
  }
  void bind_parameter(std::size_t, uniorm::sql_value const&) override {}
  void bind_column(
    std::size_t, uniorm::backend::column_buffer const&) override {}
  void bind_batch_params(std::vector<uniorm::params> const&) override {}
  void set_row_array_size(std::size_t) override {}
  void set_paramset_size(std::size_t) override {}
  void reset() override {}
  bool fetch() override {
    return false;
  }
  std::size_t affected_rows() const override {
    return 0;
  }
  std::vector<uniorm::column_info> column_meta() const override {
    return {};
  }
  std::size_t rows_fetched() const override {
    return 0;
  }
  std::string read_long_text(std::size_t, std::size_t) override {
    throw std::logic_error("the heartbeat fake fetches no text");
  }
  std::vector<std::byte> read_long_bytes(std::size_t, std::size_t) override {
    throw std::logic_error("the heartbeat fake fetches no bytes");
  }
  uniorm::backend::batch_writer_iface& prepare_batch() override {
    throw std::logic_error("the heartbeat fake writes no batches");
  }

private:
  std::shared_ptr<beat_state> state_;
};

class beat_connection : public uniorm::backend::connection_iface {
public:
  explicit beat_connection(std::shared_ptr<beat_state> state)
    : state_(std::move(state)) {}

  void open(std::string_view) override {}
  void open(uniorm::connection_spec const&) override {}
  void close() override {}
  bool is_open() const noexcept override {
    return true;
  }
  void set_autocommit(bool) override {}
  void commit() override {}
  void rollback() override {}
  uniorm::backend::capabilities caps() const noexcept override {
    return {};
  }
  std::string dbms_name() const override {
    return "beat";
  }
  std::unique_ptr<uniorm::backend::statement_iface>
  create_statement() override {
    return std::make_unique<beat_statement>(state_);
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<beat_state> state_;
};

// Registration and maintenance are the two threads that meet over the
// scheduler's own bookkeeping: the worker walks every pool's next deadline
// while another thread is still adding one, and a pool added since the last
// pass may die before the pass that would have pruned it. No CHECK can see a
// torn deadline here, which is why the shape is worth keeping: it is the
// schedule a thread sanitizer gets to examine.
void test_registration_meets_the_maintainer() {
  auto state = std::make_shared<beat_state>();
  uniorm::backend::registry::instance().register_backend(
    "beat", [state] { return std::make_unique<beat_connection>(state); });

  uniorm::pool_options opts;
  opts.target = "beat://x";
  opts.size = 1;
  opts.acquire_timeout = std::chrono::milliseconds(500);
  opts.heartbeat_interval = std::chrono::milliseconds(1);

  uniorm::connection_pool kept(opts);
  {
    uniorm::pooled_connection lease = kept.acquire();
  }  // handed back, so there is an idle connection to heartbeat

  std::size_t latecomers_beat = 0;
  for (int round = 0; round < 32; ++round) {
    auto latecomer = std::make_unique<uniorm::connection_pool>(opts);
    {
      uniorm::pooled_connection lease = latecomer->acquire();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (latecomer->heartbeats_executed() > 0) {
      ++latecomers_beat;
    }
    latecomer.reset();
  }

  CHECK(kept.heartbeats_executed() >= 1);
  CHECK(latecomers_beat >= 1);
  CHECK(state->executes.load() >= 1);
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
  test_registry_waiter_does_not_stall_other_keys();
  test_registration_meets_the_maintainer();
}
