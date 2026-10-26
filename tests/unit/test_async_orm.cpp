#include "async_executors.hpp"
#include "check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <uniorm/async.hpp>
#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/builder/expression.hpp>
#include <uniorm/connection_spec.hpp>
#include <uniorm/error.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/params.hpp>
#include <uniorm/pool.hpp>
#include <uniorm/types.hpp>

using namespace uniorm;
using uniorm::test::hand_pump;
using uniorm::test::same_thread;

namespace {

struct contact {
  std::string id;
  std::string name;
};

using result_rows = std::vector<std::vector<std::string>>;

// What the recorded statements were, in the order their bodies ran: the queue's
// whole promise is written down here.
struct async_state {
  std::vector<std::string> executed;
  // Statements and commit-mode changes in one line, because a transaction's
  // ordering against a write is a fact about both.
  std::vector<std::string> log;
  std::vector<column_info> result_columns;
  result_rows rows;
  bool fail_next_execute = false;
};

class async_statement : public backend::statement_iface {
public:
  explicit async_statement(std::shared_ptr<async_state> state)
    : state_(std::move(state))
  {}

  void prepare(std::string_view sql) override {
    sql_ = std::string(sql);
  }

  void execute() override {
    if (state_->fail_next_execute) {
      state_->fail_next_execute = false;
      throw std::runtime_error("injected execute failure");
    }
    state_->executed.push_back(sql_);
    state_->log.push_back(sql_);
    cursor_ = 0;
  }

  void bind_parameter(std::size_t, sql_value const&) override {}
  void bind_batch_params(std::vector<params> const&) override {}
  backend::batch_writer_iface& prepare_batch() override {
    throw std::logic_error("the fake writes row by row");
  }
  void bind_column(
    std::size_t index, backend::column_buffer const& buffer) override {
    if (bound_.size() < index) {
      bound_.resize(index);
    }
    bound_[index - 1] = buffer;
  }

  // Every scripted value is text, so a fetch is a memcpy into the block the
  // caller bound -- which is all a driver ever does.
  bool fetch() override {
    std::size_t const remaining = state_->rows.size() - cursor_;
    fetched_ = std::min(row_array_size_, remaining);
    for (std::size_t row = 0; row < fetched_; ++row) {
      auto const& values = state_->rows.at(cursor_ + row);
      for (std::size_t column = 0; column < bound_.size(); ++column) {
        auto const& text = values.at(column);
        auto const& buffer = bound_[column];
        if (text.size() >= buffer.capacity) {
          throw std::logic_error("scripted text exceeds the fetch buffer");
        }
        char* slot = static_cast<char*>(buffer.data) + row * buffer.capacity;
        std::memcpy(slot, text.data(), text.size());
        buffer.indicator[row] = static_cast<std::int64_t>(text.size());
      }
    }
    cursor_ += fetched_;
    return fetched_ != 0;
  }

  // A chunk reports its own row count, so a sweep's total is the sum of chunks.
  std::size_t affected_rows() const override {
    return paramset_size_;
  }
  std::vector<column_info> column_meta() const override {
    return state_->result_columns;
  }
  void set_row_array_size(std::size_t size) override {
    row_array_size_ = size;
  }
  std::size_t rows_fetched() const override {
    return fetched_;
  }
  void set_paramset_size(std::size_t size) override {
    paramset_size_ = size;
  }
  std::string read_long_text(std::size_t, std::size_t) override {
    throw std::logic_error("unexpected long text read");
  }
  std::vector<std::byte> read_long_bytes(std::size_t, std::size_t) override {
    throw std::logic_error("unexpected long bytes read");
  }
  void reset() override {
    // sql_ survives: a cache hit rebinds without preparing again.
    bound_.clear();
    cursor_ = 0;
    fetched_ = 0;
    paramset_size_ = 1;
  }

private:
  std::shared_ptr<async_state> state_;
  std::string sql_;
  std::vector<backend::column_buffer> bound_;
  std::size_t row_array_size_ = 1;
  std::size_t paramset_size_ = 1;
  std::size_t cursor_ = 0;
  std::size_t fetched_ = 0;
};

class async_connection : public backend::connection_iface {
public:
  explicit async_connection(std::shared_ptr<async_state> state)
    : state_(std::move(state))
  {}

  void open(std::string_view) override {
    open_ = true;
  }
  // The pieces arrive unformatted and this fake has no dialect to format them
  // into, so it accepts any of them and opens anyway.
  void open(connection_spec const&) override {
    open_ = true;
  }
  void close() override {
    open_ = false;
  }
  bool is_open() const noexcept override {
    return open_;
  }
  void set_autocommit(bool enabled) override {
    state_->log.push_back(enabled ? "autocommit on" : "autocommit off");
  }
  void commit() override {
    state_->log.push_back("commit");
  }
  void rollback() override {
    state_->log.push_back("rollback");
  }
  backend::capabilities caps() const noexcept override {
    return { false, false };
  }
  std::string dbms_name() const override {
    return "PostgreSQL";
  }
  std::unique_ptr<backend::statement_iface> create_statement() override {
    return std::make_unique<async_statement>(state_);
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<async_state> state_;
  bool open_ = false;
};

pool_options register_async_backend(
  std::shared_ptr<async_state> const& state) {
  static std::size_t next_backend = 0;
  std::string const scheme = "asyncfake" + std::to_string(next_backend++);
  backend::registry::instance().register_backend(scheme, [state] {
    return std::make_unique<async_connection>(state);
  });
  pool_options options;
  options.target = scheme + "://test;DSN=" + scheme;
  options.size = 1;
  options.acquire_timeout = std::chrono::milliseconds(200);
  options.heartbeat_interval = std::chrono::milliseconds(0);
  return options;
}

std::string const& recipe(pool_options const& options) {
  return std::get<std::string>(options.target);
}

// The fake is registered under its own scheme, and a connection_spec names the
// scheme rather than a whole string.
std::string scheme_of(pool_options const& options) {
  auto const& text = recipe(options);
  return text.substr(0, text.find("://"));
}

struct async_fixture {
  std::shared_ptr<async_state> state = std::make_shared<async_state>();
  pool_options options = register_async_backend(state);
  connection_pool pool{ options };
};

void map_contacts(async::orm& session) {
  auto mapping = session.map<contact>("contacts");
  mapping.column("id", &contact::id).column("name", &contact::name);
}

void script_two_contacts(async_state& state) {
  state.result_columns = { { "id", sql_type::varchar, 24, false },
    { "name", sql_type::varchar, 24, false } };
  state.rows = { { "7", "Ada" }, { "8", "Lin" } };
}

// A detached failure is a broken test, not something a case acts on.
void quiet(std::exception_ptr failure) {
  CHECK(!failure);
}

// Reads one value through the session from inside a coroutine, so the awaited
// path enters the queue the way any other request does.
async::task<void> awaited_write(async::orm& session) {
  std::size_t const changed = co_await session.execute_update("UPDATE second");
  CHECK(changed == 1);
  co_return;
}

// The shape the documentation shows: a handle that moves out of an awaited
// begin(), writes on the same session, and closes itself with an operation.
async::task<void> ship(async::orm& session) {
  auto tx = co_await session.begin();
  co_await session.execute_update("UPDATE shipped");
  co_await tx.commit();
  CHECK(!tx.active());
  co_return;
}

// The count only exists once the continuation has run, so where that lands is
// visible from outside the coroutine.
async::task<void> read_into(async::orm& session, std::size_t& seen) {
  auto const rows = co_await session.query<contact>().all();
  seen = rows.size();
  co_return;
}

// Two reads awaited back to back, so the moment between them is a live chain
// holding no turn.
async::task<void> read_twice(async::orm& session, std::size_t& seen) {
  auto const first =
    co_await session.query<contact>("SELECT id, name FROM contacts");
  auto const second =
    co_await session.query<contact>("SELECT id, name FROM contacts");
  seen = first.size() + second.size();
  co_return;
}

void test_operations_take_their_turn_in_order() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);

  auto head = session.execute_update("UPDATE one");
  auto middle = session.execute_update("UPDATE two");
  auto tail = session.execute_update("UPDATE three");
  head.start(quiet);
  middle.start(quiet);
  tail.start(quiet);
  // One connection, so one request got a thread; the rest wait on the session
  // and hold no thread at all.
  CHECK(pump.queued() == 1);
  CHECK(session.pending() == 3);
  CHECK(fixture.state->executed.empty());

  pump.run_one();
  CHECK(session.pending() == 2);
  CHECK(pump.queued() == 1);
  CHECK(fixture.state->executed.size() == 1);
  CHECK(pump.run_all() == 2);
  CHECK(pump.queued() == 0);
  CHECK(session.pending() == 0);
  std::vector<std::string> const expected{ "UPDATE one", "UPDATE two",
    "UPDATE three" };
  CHECK(fixture.state->executed == expected);
}

void test_an_awaited_request_waits_behind_a_detached_one() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);
  same_thread inline_run;

  auto first = session.execute_update("UPDATE first");
  first.start(quiet);
  CHECK(pump.queued() == 1);

  // Driving the parent on an executor that runs it at once is what lets the
  // await enter the queue while the caller still holds its thread.
  auto parent = awaited_write(session);
  parent.start(inline_run, quiet);
  CHECK(session.pending() == 2);
  CHECK(pump.queued() == 1);
  CHECK(fixture.state->executed.empty());

  pump.run_all();
  std::vector<std::string> const expected{ "UPDATE first", "UPDATE second" };
  CHECK(fixture.state->executed == expected);
  CHECK(session.pending() == 0);
}

void test_the_session_decides_where_a_body_runs() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);
  same_thread named;

  auto work = session.execute_update("UPDATE here");
  // The session made a decision, so the executor named here has no say. It
  // cannot run the body here either: block() on such a session would park this
  // thread while the work it waits for sits unpumped.
  work.start(named, quiet);
  CHECK(named.submissions() == 0);
  CHECK(pump.queued() == 1);
  pump.run_all();
  CHECK(named.submissions() == 0);
  CHECK(fixture.state->executed.size() == 1);
}

// resume_on answers the other direction: the body ran on the session's
// executor, and the continuation goes to the one named for resuming instead of
// staying on the thread that finished the read.
void test_a_continuation_lands_on_the_executor_named_for_resuming() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump runs;
  hand_pump resumes;
  same_thread here;
  session.run_on(runs);
  session.resume_on(resumes);
  map_contacts(session);
  script_two_contacts(*fixture.state);

  std::size_t seen = 0;
  auto work = read_into(session, seen);
  work.start(here, quiet);
  CHECK(runs.queued() == 1);
  CHECK(resumes.queued() == 0);

  runs.run_one();
  // The read is over and its result is in the core, but nothing has continued
  // with it: the hand-off is the test's to make.
  CHECK(fixture.state->executed.size() == 1);
  CHECK(resumes.queued() == 1);
  CHECK(seen == 0);
  CHECK(session.pending() == 0);

  resumes.run_one();
  CHECK(seen == 2);
}

void test_a_full_queue_refuses_without_starting_anything() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);
  session.submit_limit(2);
  CHECK(session.submit_limit() == 2);

  auto head = session.execute_update("UPDATE one");
  auto middle = session.execute_update("UPDATE two");
  auto extra = session.execute_update("UPDATE three");
  head.start(quiet);
  middle.start(quiet);
  CHECK_THROWS(extra.start(quiet), uniorm::submit_overflow);
  CHECK(pump.queued() == 1);
  CHECK(session.pending() == 2);
  CHECK(fixture.state->executed.empty());

  // Nothing was put in flight and the task stayed undriven, so trying it again
  // once the queue has room is a legal shape.
  pump.run_all();
  CHECK(session.pending() == 0);
  extra.start(quiet);
  pump.run_all();
  std::vector<std::string> const expected{ "UPDATE one", "UPDATE two",
    "UPDATE three" };
  CHECK(fixture.state->executed == expected);
}

// A limit of zero would leave a session with no room for anything, so it is
// read as one turn: the request made now gets in, the next one is refused.
void test_a_submit_limit_of_zero_means_one_turn() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);
  session.submit_limit(0);
  CHECK(session.submit_limit() == 1);

  auto only = session.execute_update("UPDATE only");
  auto extra = session.execute_update("UPDATE refused");
  only.start(quiet);
  CHECK_THROWS(extra.start(quiet), uniorm::submit_overflow);
  CHECK(session.pending() == 1);

  pump.run_all();
  CHECK(fixture.state->executed == std::vector<std::string>{ "UPDATE only" });
}

void test_a_builder_terminal_reads_through_the_session() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);
  map_contacts(session);
  script_two_contacts(*fixture.state);
  std::string const select = session.sync().query<contact>().build_select();

  auto const rows = session.query<contact>().all().block();
  CHECK(rows.size() == 2);
  CHECK(rows.at(0).id == "7");
  CHECK(rows.at(0).name == "Ada");
  CHECK(rows.at(1).id == "8");
  CHECK(fixture.state->executed == std::vector<std::string>{ select });

  fixture.state->executed.clear();
  fixture.state->rows = { { "8", "Lin" } };
  auto const single =
    session.query<contact>().where(eq(&contact::id, "8")).one().block();
  CHECK(single.has_value());
  CHECK(single && single->name == "Lin");
  CHECK(fixture.state->executed.size() == 1);
  CHECK(fixture.state->executed.at(0).find("WHERE") != std::string::npos);

  // A hand-written statement reads through the same mapping.
  fixture.state->executed.clear();
  auto const written = session
    .query<contact>("SELECT id, name FROM contacts", params("x")).block();
  CHECK(written.size() == 1);
  CHECK(written.at(0).id == "8");
  CHECK(fixture.state->executed ==
    std::vector<std::string>{ "SELECT id, name FROM contacts" });
}

// The catalog side keeps no asynchronous door on purpose: a coroutine that
// wants a mapping validated calls through sync() and blocks that thread, which
// is the one accepted blocking point of this API.
void test_the_catalog_side_stays_synchronous() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);
  map_contacts(session);
  CHECK_THROWS(
    session.sync().validate(), uniorm::backend::capability_not_supported);
  CHECK(session.pending() == 0);

  // Nothing queued, and the write side carries on afterwards.
  CHECK(session.execute_update("UPDATE after validate").block() == 1);
}

// Each forwarded knob writes the one wrapped object: sync() reports the value
// back, and the operation the queue runs obeys it.
void test_the_forwarded_knobs_write_the_wrapped_session() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);
  map_contacts(session);

  session.row_array_size(4);
  session.paramset_size(3);
  session.identifier_case(dialect::identifier_case::upper);
  CHECK(session.sync().row_array_size() == 4);
  CHECK(session.sync().paramset_size() == 3);
  CHECK(session.sync().identifier_case() == dialect::identifier_case::upper);
  CHECK(session.row_array_size() == 4);
  CHECK(session.paramset_size() == 3);
  CHECK(session.auto_commit());

  // Three rows and a paramset of three is one chunk, which only happens if the
  // knob landed on the connection the body uses.
  std::vector<contact> const rows{ { "1", "Ada" }, { "2", "Lin" },
    { "3", "Grace" } };
  CHECK(session.insert(rows).block() == 3);
  CHECK(fixture.state->executed.size() == 1);
}

// The session takes a connection itself: no synchronous object appears in this
// program, and the mapping written through the forwarded map() is the one the
// queued statement renders from.
void test_a_session_builds_from_a_connection_string() {
  async_fixture fixture;
  connection_pool_registry::instance().configure(
    recipe(fixture.options), fixture.options);
  async::orm session(recipe(fixture.options));
  same_thread run;
  session.run_on(run);
  map_contacts(session);
  script_two_contacts(*fixture.state);

  CHECK(session.auto_commit());
  auto const rows = session.query<contact>().all().block();
  CHECK(rows.size() == 2);
  CHECK(rows.at(1).name == "Lin");
  CHECK(fixture.state->executed.size() == 1);
  CHECK(fixture.state->executed.at(0).find("contacts") != std::string::npos);
  CHECK(session.pending() == 0);
}

// The same three ways in as the synchronous object, with the same lease
// behaviour: a spec reaches the backend it names, and a session taken over from
// a uniorm::orm brings its mapping and its lease along.
void test_a_session_builds_from_a_spec_and_from_a_sync_object() {
  async_fixture fixture;
  same_thread run;
  connection_spec spec{ scheme_of(fixture.options), "spec target", "who", "" };
  connection_pool_registry::instance().configure(spec, fixture.options);

  async::orm from_spec(spec);
  from_spec.run_on(run);
  map_contacts(from_spec);
  CHECK(from_spec.execute_update("UPDATE through the spec").block() == 1);
  from_spec.disconnect().block();
  from_spec.connect(spec).block();
  CHECK(from_spec.execute_update("UPDATE after reconnect").block() == 1);

  uniorm::orm sync_db(fixture.pool);
  async::orm taken(std::move(sync_db));
  taken.run_on(run);
  map_contacts(taken);
  auto following =
    taken.execute_update("UPDATE through the taken-over session");
  CHECK(following.block() == 1);

  std::vector<std::string> const expected{ "UPDATE through the spec",
    "UPDATE after reconnect", "UPDATE through the taken-over session" };
  CHECK(fixture.state->executed == expected);
}

void test_a_failing_body_frees_the_session_for_the_next_one() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);

  fixture.state->fail_next_execute = true;
  auto broken = session.execute_update("UPDATE broken");
  CHECK_THROWS(broken.block(), std::runtime_error);
  CHECK(run.submissions() == 1);
  CHECK(session.pending() == 0);

  auto following = session.execute_update("UPDATE next");
  CHECK(following.block() == 1);
  CHECK(fixture.state->executed == std::vector<std::string>{ "UPDATE next" });
}

void test_a_batch_sweep_hands_over_one_operation() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);
  map_contacts(session);
  session.paramset_size(2);

  std::vector<contact> const rows{ { "1", "Ada" }, { "2", "Lin" },
    { "3", "Grace" } };
  auto swept = session.insert(rows);
  swept.start(quiet);
  // Three rows in two chunks and still one hand-off: the whole scan is the
  // operation the queue owns, exactly as the synchronous call owns it.
  CHECK(pump.queued() == 1);
  CHECK(session.pending() == 1);
  pump.run_one();
  CHECK(fixture.state->executed.size() == 2);
  CHECK(session.pending() == 0);
  CHECK(pump.queued() == 0);
}

// Both faces of the two remaining writes are the call the synchronous object
// makes: the statements it sent, one for one, and the same counts back.
void test_the_write_faces_send_what_the_sync_object_sends() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);
  auto mapping = session.map<contact>("contacts");
  mapping.primary_key("id", &contact::id).column("name", &contact::name);
  std::vector<contact> const rows{ { "1", "Ada" }, { "2", "Lin" } };

  auto& sync = session.sync();
  std::size_t const updated = sync.update(rows);
  std::size_t const removed = sync.remove(rows);
  std::vector<std::string> const sent = fixture.state->executed;
  fixture.state->executed.clear();
  // The fake cannot tally a whole parameter array, so a two-row sweep is two
  // statements each way.
  CHECK(sent.size() == 4);

  CHECK(session.update(rows).block() == updated);
  CHECK(session.remove(rows).block() == removed);
  CHECK(fixture.state->executed == sent);

  fixture.state->executed.clear();
  std::size_t answered_updated = 0;
  std::size_t answered_removed = 0;
  session.update(rows, [&](std::size_t changed, std::exception_ptr failure) {
    CHECK(!failure);
    answered_updated = changed;
  });
  session.remove(rows, [&](std::size_t changed, std::exception_ptr failure) {
    CHECK(!failure);
    answered_removed = changed;
  });
  CHECK(answered_updated == updated);
  CHECK(answered_removed == removed);
  CHECK(fixture.state->executed == sent);
}

void test_connect_and_the_transaction_knob_run_as_operations() {
  async_fixture fixture;
  connection_pool_registry::instance().configure(
    recipe(fixture.options), fixture.options);
  async::orm session;
  same_thread run;
  session.run_on(run);

  session.connect(recipe(fixture.options)).block();
  CHECK(session.execute_update("UPDATE after connect").block() == 1);
  CHECK(fixture.state->executed ==
    std::vector<std::string>{ "UPDATE after connect" });
  fixture.state->log.clear();
  session.auto_commit(false).block();
  session.commit().block();
  session.rollback().block();

  // Each of the three reached the connection once, in the order they were
  // made: statements and mode changes share one line on purpose.
  std::vector<std::string> const trail{ "autocommit off", "commit",
    "rollback" };
  CHECK(fixture.state->log == trail);
  session.disconnect().block();
}

// The documented shape, run end to end: begin, write, commit, and each of the
// three is one operation on the same session.
void test_a_coroutine_opens_and_closes_a_transaction() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread now;
  session.run_on(now);
  // Taking the lease out of the pool already put the connection in autocommit;
  // that one belongs to the synchronous path, not to this case.
  fixture.state->log.clear();

  ship(session).block(now);
  std::vector<std::string> const expected{ "autocommit off", "UPDATE shipped",
    "commit", "autocommit on" };
  CHECK(fixture.state->log == expected);
  CHECK(session.pending() == 0);
}

void test_a_commit_waits_its_turn_behind_the_write_it_closes() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread now;
  session.run_on(now);
  auto tx = session.begin().block();
  fixture.state->log.clear();
  hand_pump pump;
  session.run_on(pump);

  auto write = session.execute_update("UPDATE last");
  write.start(quiet);
  auto closing = tx.commit();
  closing.start(quiet);
  // A commit is an operation like any other, so it queues behind the write
  // whose outcome it decides rather than reaching around it.
  CHECK(pump.queued() == 1);
  CHECK(session.pending() == 2);
  CHECK(fixture.state->log.empty());
  pump.run_all();
  std::vector<std::string> const expected{ "UPDATE last", "commit",
    "autocommit on" };
  CHECK(fixture.state->log == expected);
  CHECK(!tx.active());
}

void test_an_uncommitted_transaction_rolls_back_when_it_goes_out_of_scope() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread now;
  session.run_on(now);
  fixture.state->log.clear();
  {
    auto tx = session.begin().block();
    CHECK(tx.active());
    CHECK(session.execute_update("UPDATE never committed").block() == 1);
  }
  std::vector<std::string> const expected{ "autocommit off",
    "UPDATE never committed", "rollback", "autocommit on" };
  CHECK(fixture.state->log == expected);
  CHECK(session.pending() == 0);
}

// No run_on, so the write is still in flight on the default pool when the
// handle goes out of scope, and two operations on one connection is the one
// thing the session exists to prevent: the rollback waits for its turn.
void test_the_fallback_rollback_waits_for_the_session_to_go_quiet() {
  async_fixture fixture;
  {
    async::orm session(fixture.pool);
    fixture.state->log.clear();
    auto tx = session.begin().block();
    session.execute_update("UPDATE in flight").start(quiet);
  }
  std::vector<std::string> const expected{ "autocommit off",
    "UPDATE in flight", "rollback", "autocommit on" };
  CHECK(fixture.state->log == expected);
}

// Outliving the session is the other order, and it is legal too: the session
// closes what it handed out on the way down, while the connection that has to
// serve that rollback is still alive. What is left of the handle afterwards has
// nothing to reach, so it takes no operation and its destructor does nothing.
void test_a_transaction_handle_may_outlive_its_session() {
  async_fixture fixture;
  fixture.state->log.clear();
  {
    auto handle = [&fixture] {
      async::orm session(fixture.pool);
      same_thread run;
      session.run_on(run);
      fixture.state->log.clear();
      auto opened = session.begin().block();
      CHECK(session.execute_update("UPDATE never closed").block() == 1);
      return opened;
    }();
    CHECK(!handle.active());
    CHECK_THROWS(handle.commit(), uniorm::uniorm_error);
    CHECK_THROWS(handle.rollback(), uniorm::uniorm_error);
  }
  std::vector<std::string> const expected{ "autocommit off",
    "UPDATE never closed", "rollback", "autocommit on" };
  CHECK(fixture.state->log == expected);
}

// No run_on: the body goes to the default pool, and the destructor is what
// turns "it will finish" into "it has finished".
void test_the_destructor_waits_for_handed_over_work() {
  async_fixture fixture;
  {
    async::orm session(fixture.pool);
    session.execute_update("UPDATE last").start(quiet);
  }
  CHECK(fixture.state->executed == std::vector<std::string>{ "UPDATE last" });
}

// An executor that takes nothing: no body ran to answer, so the failure
// handler the request carries is the only place left to answer from.
class refusing final : public uniorm::async::executor {
public:
  void submit(std::function<void()>) override {
    throw std::runtime_error("executor refused");
  }
};

void test_a_refused_hand_off_is_still_answered() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  refusing nowhere;
  session.run_on(nowhere);
  map_contacts(session);

  std::exception_ptr bad;
  session.query<contact>("SELECT id, name FROM contacts",
    [&](std::vector<contact> rows, std::exception_ptr failure) {
      CHECK(rows.empty());
      bad = failure;
    });
  CHECK(bad);
  CHECK_THROWS(std::rethrow_exception(bad), std::runtime_error);
  CHECK(session.pending() == 0);
}

// The failure handler is the last thing between a failed operation and the
// session it belongs to, and both places that run it forbid throwing: the
// final awaiter is noexcept, and a starter that unwinds leaves the queue
// mid-walk. So what a handler throws stops where it was thrown, and the turn
// still comes back.
void test_a_throwing_handler_leaves_the_session_usable() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);
  map_contacts(session);
  auto loud = [](std::exception_ptr failure) {
    CHECK(failure);
    throw std::runtime_error("the handler threw");
  };

  // Reached from the final awaiter, because the body itself threw.
  fixture.state->fail_next_execute = true;
  session.execute_update("UPDATE broke").start(loud);
  CHECK(session.pending() == 0);

  // Reached from inside the queue's own walk, because the executor refused the
  // hand-off before any body ran.
  refusing nowhere;
  session.run_on(nowhere);
  session.execute_update("UPDATE refused").start(loud);
  CHECK(session.pending() == 0);

  // Both turns came back, so the session still takes the next request.
  session.run_on(run);
  CHECK(session.execute_update("UPDATE after both").block() == 1);
  CHECK(fixture.state->executed ==
    std::vector<std::string>{ "UPDATE after both" });
}

// The callback face answers with the rows the task face would have returned,
// from the statement the task face would have sent.
void test_a_callback_answers_with_the_rows_it_read() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);
  map_contacts(session);
  script_two_contacts(*fixture.state);
  std::string const select = session.sync().query<contact>().build_select();

  std::vector<contact> found;
  // Poisoned first, so a callback that never ran cannot look like a failure-
  // free answer.
  std::exception_ptr bad = std::make_exception_ptr(std::logic_error("unset"));
  session.query<contact>(select,
    [&](std::vector<contact> rows, std::exception_ptr failure) {
      found = std::move(rows);
      bad = failure;
    });
  CHECK(!bad);
  CHECK(found.size() == 2);
  CHECK(found.at(0).id == "7");
  CHECK(found.at(1).name == "Lin");
  CHECK(fixture.state->executed == std::vector<std::string>{ select });
  CHECK(session.pending() == 0);
}

// A failed read answers once: nothing in the value slot, the body's own
// exception in the other -- two rethrows of it land on one address.
void test_a_callback_carries_the_failure_the_body_threw() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);
  map_contacts(session);
  fixture.state->fail_next_execute = true;

  std::vector<contact> found(1, contact{ "not", "empty" });
  std::exception_ptr bad;
  session.query<contact>("SELECT id, name FROM contacts",
    [&](std::vector<contact> rows, std::exception_ptr failure) {
      found = std::move(rows);
      bad = failure;
    });
  CHECK(found.empty());
  CHECK(bad);

  void const* first = nullptr;
  void const* second = nullptr;
  try {
    std::rethrow_exception(bad);
  } catch (std::runtime_error const& thrown) {
    first = &thrown;
    CHECK(std::string(thrown.what()) == "injected execute failure");
  }
  try {
    std::rethrow_exception(bad);
  } catch (std::runtime_error const& thrown) {
    second = &thrown;
  }
  CHECK(first == second);
  CHECK(session.pending() == 0);
}

// A callback request enters the same FIFO as any other, so its answer sees the
// write that was already waiting, and it holds exactly one turn of its own.
void test_a_callback_waits_behind_a_detached_write() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);
  map_contacts(session);
  script_two_contacts(*fixture.state);

  auto write = session.execute_update("UPDATE before the read");
  write.start(quiet);
  std::size_t reads = 0;
  session.query<contact>("SELECT id, name FROM contacts",
    [&](std::vector<contact> rows, std::exception_ptr failure) {
      CHECK(!failure);
      reads = rows.size();
    });
  CHECK(pump.queued() == 1);
  CHECK(session.pending() == 2);
  CHECK(reads == 0);

  pump.run_all();
  CHECK(reads == 2);
  std::vector<std::string> const expected{ "UPDATE before the read",
    "SELECT id, name FROM contacts" };
  CHECK(fixture.state->executed == expected);
}

// A callback request the queue has no room for is refused the way a task is:
// at the call site, with nothing started and no answer to wait for.
void test_a_full_queue_refuses_a_callback_request_too() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);
  session.submit_limit(1);
  map_contacts(session);
  script_two_contacts(*fixture.state);

  bool answered = false;
  session.query<contact>("SELECT id, name FROM contacts",
    [&](std::vector<contact>, std::exception_ptr) { answered = true; });
  CHECK(session.pending() == 1);
  CHECK_THROWS(session.query<contact>("SELECT id, name FROM contacts",
                [&](std::vector<contact>, std::exception_ptr) {
                  answered = true;
                }),
    uniorm::submit_overflow);
  CHECK(session.pending() == 1);

  pump.run_all();
  CHECK(answered);
  CHECK(fixture.state->executed ==
    std::vector<std::string>{ "SELECT id, name FROM contacts" });
}

// Nobody catches what the caller's own callback throws: the turn is freed all
// the same, so the next request still runs.
void test_a_callback_that_throws_leaves_the_session_usable() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);
  map_contacts(session);
  script_two_contacts(*fixture.state);

  session.query<contact>("SELECT id, name FROM contacts",
    [](std::vector<contact>, std::exception_ptr) {
      throw std::runtime_error("the callback threw");
    });
  CHECK(session.pending() == 0);
  auto following =
    session.execute_update("UPDATE after a throwing callback");
  CHECK(following.block() == 1);
}

// A callback that is not there cannot answer, and inside the body its
// bad_function_call would only be swallowed: the request is refused where it is
// made, which is the one place both faces agree on.
void test_an_empty_callback_is_refused_at_the_call() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread run;
  session.run_on(run);
  map_contacts(session);

  std::function<void(std::vector<contact>, std::exception_ptr)> absent;
  CHECK_THROWS(session.query<contact>(
                 "SELECT id, name FROM contacts", absent),
    uniorm::uniorm_error);
  CHECK(session.pending() == 0);
  CHECK(fixture.state->executed.empty());
}

// A chain that cannot get its next turn used to unwind out of the worker that
// was running it: the pool lost that thread, and every session with work left
// in it lost the answer. The refusal belongs to the await now, so a body
// catches it the way it catches a failing statement.
async::task<void> refused_at_the_await(async::orm& session,
  std::atomic<bool>& caught, std::binary_semaphore& settled) {
  try {
    co_await session.execute_update("UPDATE never queued");
    caught = false;
  } catch (uniorm::submit_overflow const&) {
    caught = true;
  }
  settled.release();
  co_return;
}

void test_a_chain_takes_a_refused_turn_at_its_await() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);
  session.submit_limit(1);
  map_contacts(session);

  // One request holds the only turn, and the pump is what keeps it there.
  session.execute_update("UPDATE in flight").start(quiet);
  CHECK(session.pending() == 1);

  // The chain is not a session operation, so the executor named on the drive
  // action is where its body runs: the default pool.
  std::atomic<bool> caught{ false };
  std::binary_semaphore settled{ 0 };
  auto chain = refused_at_the_await(session, caught, settled);
  chain.start(uniorm::async::default_executor(), quiet);
  settled.acquire();
  CHECK(caught.load());
  CHECK(fixture.state->executed.empty());

  pump.run_all();
  session.wait_all_operations();
  CHECK(session.pending() == 0);
  CHECK(fixture.state->executed ==
    std::vector<std::string>{ "UPDATE in flight" });
}

// The answer is inside the body, so with no run_on the destructor waits on a
// worker that has not finished calling back: ~orm does not come back until the
// callback has come back. The callback parks where the test can see it, and
// only the test can let it go, so there is no timing assumption in the way.
void test_the_destructor_waits_for_a_callback_to_be_answered() {
  async_fixture fixture;
  std::binary_semaphore parked{ 0 };
  std::binary_semaphore go{ 0 };
  std::atomic<bool> answered{ false };
  auto session = std::make_unique<async::orm>(fixture.pool);
  map_contacts(*session);
  std::vector<contact> const rows{ { "9", "Ada" } };
  session->insert(rows, [&](std::size_t added, std::exception_ptr failure) {
    CHECK(!failure);
    CHECK(added == 1);
    parked.release();
    go.acquire();
    answered = true;
  });

  // Parked inside the callback: the turn is held by something that cannot
  // finish until the test says so, which is the state ~orm has to wait out.
  parked.acquire();
  CHECK(!answered.load());
  std::thread closer([raw = session.release()] {
    delete raw;
  });
  go.release();
  closer.join();
  CHECK(answered.load());
  CHECK(fixture.state->executed.size() == 1);
  CHECK(fixture.state->executed.at(0).rfind("INSERT INTO ", 0) == 0);
}

// The same barrier with the session still open: it covers a detached write and
// a callback that has not answered yet, and it is not a shutdown -- the queue
// takes the next request afterwards.
void test_the_barrier_covers_every_accepted_request() {
  async_fixture fixture;
  std::binary_semaphore parked{ 0 };
  std::binary_semaphore go{ 0 };
  std::atomic<bool> answered{ false };
  async::orm session(fixture.pool);
  map_contacts(session);
  std::vector<contact> const rows{ { "10", "Boole" } };
  session.execute_update("UPDATE before the wait").start(quiet);
  session.insert(rows, [&](std::size_t added, std::exception_ptr failure) {
    CHECK(!failure);
    CHECK(added == 1);
    parked.release();
    go.acquire();
    answered = true;
  });
  parked.acquire();
  CHECK(!answered.load());
  CHECK(session.pending() > 0);

  // Released from another thread: the barrier may well start while the
  // callback is still in there, and it cannot come back before the answer.
  std::thread opener([&go] {
    go.release();
  });
  session.wait_all_operations();
  opener.join();
  CHECK(answered.load());
  CHECK(session.pending() == 0);
  CHECK(fixture.state->executed.size() == 2);
  CHECK(fixture.state->executed.at(0) == "UPDATE before the wait");
  CHECK(fixture.state->executed.at(1).rfind("INSERT INTO ", 0) == 0);

  auto following = session.execute_update("UPDATE after the wait");
  CHECK(following.block() == 1);
  CHECK(fixture.state->executed.size() == 3);
}

// The refused request never entered the queue, so the barrier has no answer of
// its own to wait for: it comes back with that callback still unanswered.
void test_the_barrier_does_not_wait_on_a_refused_request() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  hand_pump pump;
  session.run_on(pump);
  session.submit_limit(1);
  map_contacts(session);
  script_two_contacts(*fixture.state);

  bool accepted_answered = false;
  bool refused_answered = false;
  session.query<contact>("SELECT id, name FROM contacts",
    [&](std::vector<contact>, std::exception_ptr) {
      accepted_answered = true;
    });
  CHECK_THROWS(session.query<contact>("SELECT id, name FROM contacts",
                [&](std::vector<contact>, std::exception_ptr) {
                  refused_answered = true;
                }),
    uniorm::submit_overflow);

  pump.run_all();
  session.wait_all_operations();
  CHECK(accepted_answered);
  CHECK(!refused_answered);
  CHECK(session.pending() == 0);
}

// What the barrier does not cover: a chain that has handed its turn back and
// not yet taken the next one. The queue is empty here, so the barrier comes
// back with that coroutine still alive and one read short of its end.
void test_a_chain_between_two_requests_holds_no_turn() {
  async_fixture fixture;
  async::orm session(fixture.pool);
  same_thread inline_run;
  hand_pump resumer;
  session.run_on(inline_run);
  session.resume_on(resumer);
  map_contacts(session);
  script_two_contacts(*fixture.state);

  std::size_t seen = 0;
  auto parent = read_twice(session, seen);
  parent.start(inline_run, quiet);

  CHECK(fixture.state->executed.size() == 1);
  CHECK(session.pending() == 0);
  CHECK(resumer.queued() == 1);

  session.wait_all_operations();
  CHECK(seen == 0);
  CHECK(resumer.queued() == 1);

  resumer.run_all();
  CHECK(fixture.state->executed.size() == 2);
  CHECK(seen == 4);
  CHECK(session.pending() == 0);
}

}  // namespace

void test_async_orm() {
  test_operations_take_their_turn_in_order();
  test_an_awaited_request_waits_behind_a_detached_one();
  test_the_session_decides_where_a_body_runs();
  test_a_continuation_lands_on_the_executor_named_for_resuming();
  test_a_full_queue_refuses_without_starting_anything();
  test_a_submit_limit_of_zero_means_one_turn();
  test_a_builder_terminal_reads_through_the_session();
  test_the_catalog_side_stays_synchronous();
  test_the_forwarded_knobs_write_the_wrapped_session();
  test_a_session_builds_from_a_connection_string();
  test_a_session_builds_from_a_spec_and_from_a_sync_object();
  test_a_failing_body_frees_the_session_for_the_next_one();
  test_a_batch_sweep_hands_over_one_operation();
  test_the_write_faces_send_what_the_sync_object_sends();
  test_connect_and_the_transaction_knob_run_as_operations();
  test_a_coroutine_opens_and_closes_a_transaction();
  test_a_commit_waits_its_turn_behind_the_write_it_closes();
  test_an_uncommitted_transaction_rolls_back_when_it_goes_out_of_scope();
  test_the_fallback_rollback_waits_for_the_session_to_go_quiet();
  test_a_transaction_handle_may_outlive_its_session();
  test_the_destructor_waits_for_handed_over_work();
  test_a_refused_hand_off_is_still_answered();
  test_a_throwing_handler_leaves_the_session_usable();
  test_a_callback_answers_with_the_rows_it_read();
  test_a_callback_carries_the_failure_the_body_threw();
  test_a_callback_waits_behind_a_detached_write();
  test_a_full_queue_refuses_a_callback_request_too();
  test_a_callback_that_throws_leaves_the_session_usable();
  test_an_empty_callback_is_refused_at_the_call();
  test_a_chain_takes_a_refused_turn_at_its_await();
  test_the_destructor_waits_for_a_callback_to_be_answered();
  test_the_barrier_covers_every_accepted_request();
  test_the_barrier_does_not_wait_on_a_refused_request();
  test_a_chain_between_two_requests_holds_no_turn();
}
