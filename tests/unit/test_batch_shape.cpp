#include "check.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/error.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/params.hpp>
#include <uniorm/pool.hpp>
#include <uniorm/types.hpp>

using namespace uniorm;

namespace {

struct account {
  std::int64_t id = 0;
  std::string name;
};

// One entry per execute: the rows the core handed that execute as its batch.
struct batch_state {
  std::vector<std::size_t> batches;
  bool report_array_totals = true;
};

// A non-columnar backend, which is what an OCI or libpq leg is: the core's
// row-wise route is the only one it is offered, and how that route cuts the
// sweep into executes is the `array_rowcount_totals` question.
class batch_statement : public backend::statement_iface {
public:
  explicit batch_statement(std::shared_ptr<batch_state> state)
    : state_(std::move(state)) {}

  void prepare(std::string_view) override {}
  void bind_parameter(std::size_t, sql_value const&) override {}
  void bind_column(std::size_t, backend::column_buffer const&) override {}

  void bind_batch_params(std::vector<params> const& rows) override {
    bound_ = rows.size();
    state_->batches.push_back(bound_);
  }

  backend::batch_writer_iface& prepare_batch() override {
    throw std::logic_error("a backend without the columnar batch was asked"
                           " for one");
  }

  void execute() override {
    // The answer this fake gives is the flag's whole point: a backend without
    // array totals reports one parameter set's count for an execute that ran
    // the whole array.
    affected_ = state_->report_array_totals || bound_ < 2 ? bound_ : 1;
  }

  bool fetch() override {
    return false;
  }

  std::size_t affected_rows() const override {
    return affected_;
  }

  std::vector<column_info> column_meta() const override {
    return {};
  }

  void set_row_array_size(std::size_t) override {}
  std::size_t rows_fetched() const override {
    return 0;
  }
  void set_paramset_size(std::size_t) override {}
  std::string read_long_text(std::size_t, std::size_t) override {
    throw std::logic_error("unexpected long text read");
  }
  std::vector<std::byte> read_long_bytes(std::size_t, std::size_t) override {
    throw std::logic_error("unexpected long bytes read");
  }
  void reset() override {
    bound_ = 0;
    affected_ = 0;
  }

private:
  std::shared_ptr<batch_state> state_;
  std::size_t bound_ = 0;
  std::size_t affected_ = 0;
};

class batch_connection : public backend::connection_iface {
public:
  explicit batch_connection(std::shared_ptr<batch_state> state)
    : state_(std::move(state)) {}

  void open(std::string_view) override {}
  void close() override {}
  bool is_open() const noexcept override {
    return true;
  }
  void set_autocommit(bool) override {}
  void commit() override {}
  void rollback() override {}
  backend::capabilities caps() const noexcept override {
    return { /*columnar_batch=*/false,
      /*array_rowcount_totals=*/state_->report_array_totals,
      /*array_single_send=*/true, /*array_insert=*/true,
      /*array_update_delete=*/true };
  }
  std::string dbms_name() const override {
    return "batch";
  }
  std::unique_ptr<backend::statement_iface> create_statement() override {
    return std::make_unique<batch_statement>(state_);
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<batch_state> state_;
};

// The registry keeps a scheme once it has one, so each configuration of the
// fake gets a name of its own.
void register_batch_backend(
  std::shared_ptr<batch_state> state, std::string_view scheme) {
  backend::registry::instance().register_backend(std::string(scheme),
    [state] { return std::make_unique<batch_connection>(state); });
}

// A pool of one such connection. The leased connection must outlive the pool it
// came from, so a caller keeps both as its own locals.
pool_options batch_options(std::string_view scheme) {
  pool_options options;
  options.target = std::string(scheme) + "://test";
  options.size = 1;
  options.acquire_timeout = std::chrono::milliseconds{ 200 };
  options.heartbeat_interval = std::chrono::milliseconds{ 0 };
  return options;
}

std::vector<account> make_accounts(std::size_t count) {
  std::vector<account> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    rows.push_back(account{
      static_cast<std::int64_t>(index), "name" + std::to_string(index) });
  }
  return rows;
}

void map_accounts(orm& database) {
  database.map<account>("accounts")
    .primary_key("id", &account::id)
    .column("name", &account::name);
}

}  // namespace

void test_batch_shape() {
  // Named because a brace list would split across CHECK's macro arguments.
  std::vector<std::size_t> const whole_batch{ 5 };
  std::vector<std::size_t> const chunked{ 2, 2, 1 };
  std::vector<std::size_t> const one_set_at_a_time{ 1, 1, 1, 1, 1 };

  // Rows a tally-capable backend takes in one execute, and the count the core
  // reports is that execute's total.
  {
    auto state = std::make_shared<batch_state>();
    register_batch_backend(state, "batchtotals");
    connection_pool pool{ batch_options("batchtotals") };
    orm database(pool);
    map_accounts(database);
    CHECK(database.update(make_accounts(5)) == 5);
    CHECK(state->batches == whole_batch);
    state->batches.clear();
    CHECK(database.remove(make_accounts(5)) == 5);
    CHECK(state->batches == whole_batch);
  }

  // A smaller batch size still keeps one chunk per execute, and the tail chunk
  // is the rows left over rather than a full one.
  {
    auto state = std::make_shared<batch_state>();
    register_batch_backend(state, "batchchunks");
    connection_pool pool{ batch_options("batchchunks") };
    orm database(pool);
    map_accounts(database);
    database.paramset_size(2);
    CHECK(database.update(make_accounts(5)) == 5);
    CHECK(state->batches == chunked);
  }

  // Without the flag no execute's count is the array's, so a write that has to
  // tally affected rows goes one parameter set at a time. The total is the
  // same either way: it is the number of executes that differs.
  {
    auto state = std::make_shared<batch_state>();
    state->report_array_totals = false;
    register_batch_backend(state, "singleset");
    connection_pool pool{ batch_options("singleset") };
    orm database(pool);
    map_accounts(database);
    CHECK(database.update(make_accounts(5)) == 5);
    CHECK(state->batches == one_set_at_a_time);
    state->batches.clear();
    CHECK(database.remove(make_accounts(5)) == 5);
    CHECK(state->batches == one_set_at_a_time);
  }

  // An insert counts the rows it bound rather than the rows a write touched,
  // so its batch size answers to no capability.
  {
    auto state = std::make_shared<batch_state>();
    state->report_array_totals = false;
    register_batch_backend(state, "batchinsert");
    connection_pool pool{ batch_options("batchinsert") };
    orm database(pool);
    map_accounts(database);
    CHECK(database.insert(make_accounts(5)) == 5);
    CHECK(state->batches == whole_batch);
  }

  // The routing policy on a backend that reports one set's count for a whole
  // array execute -- the psqlODBC habit. automatic protects the tally by going
  // one set at a time; columnar takes the whole chunk in one execute and hands
  // back the short count the driver reports for it.
  {
    auto state = std::make_shared<batch_state>();
    state->report_array_totals = false;
    register_batch_backend(state, "batchpolicy");
    connection_pool pool{ batch_options("batchpolicy") };
    orm database(pool);
    map_accounts(database);
    CHECK(database.batch_write_policy() == orm::batch_policy::automatic);

    CHECK(database.update(make_accounts(5)) == 5);
    CHECK(state->batches == one_set_at_a_time);

    state->batches.clear();
    database.batch_write_policy(orm::batch_policy::columnar);
    CHECK(database.batch_write_policy() == orm::batch_policy::columnar);
    CHECK(database.update(make_accounts(5)) == 1);
    CHECK(state->batches == whole_batch);

    state->batches.clear();
    CHECK(database.remove(make_accounts(5)) == 1);
    CHECK(state->batches == whole_batch);
  }

  // A server with no folded shape refuses the folded policy outright.
  {
    auto state = std::make_shared<batch_state>();
    register_batch_backend(state, "batchpolicyfold");
    connection_pool pool{ batch_options("batchpolicyfold") };
    orm database(pool);
    map_accounts(database);
    database.batch_write_policy(orm::batch_policy::folded);
    CHECK_THROWS(database.update(make_accounts(5)), uniorm_error);
    CHECK_THROWS(database.remove(make_accounts(5)), uniorm_error);
  }
}
