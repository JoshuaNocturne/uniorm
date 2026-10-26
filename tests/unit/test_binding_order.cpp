#include "check.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/params.hpp>
#include <uniorm/pool.hpp>
#include <uniorm/row.hpp>
#include <uniorm/types.hpp>

using namespace uniorm;

namespace {

// Field order is id, name, city; the hand-written SELECT below lists them the
// other way.
struct contact {
  std::string id;
  std::string name;
  std::string city;
};

// The same shape, never registered: with no labels to consult it is read
// strictly by column order.
struct unregistered_row {
  std::string id;
  std::string name;
  std::string city;
};

// A user-provided constructor keeps this out of the aggregate concept on every
// implementation, and it is never registered: the typed read has no contract to
// follow and says so.
struct opaque_row {
  opaque_row() : id() {}
  std::string id;
};

// Registered, but no aggregate either: only the mapping can read it back.
struct ledger {
  ledger() = default;
  explicit ledger(std::size_t) {}
  std::string id;
  std::string name;
};

// The dispatch keys on exactly these classifications: contact satisfies both
// and registration wins, the rest have only one contract available.
static_assert(detail::aggregate_projection<contact>);
static_assert(detail::aggregate_projection<unregistered_row>);
static_assert(!detail::aggregate_projection<ledger>);
static_assert(!detail::aggregate_projection<opaque_row>);

using result_rows = std::vector<std::vector<std::string>>;

// The scripted result set, shared so a test can re-shape its labels between
// queries without handing the pool a new connection string.
struct scripted_state {
  std::vector<column_info> columns;
  result_rows rows;
  // Describes are paid per query now; counting them pins that.
  std::size_t meta_calls = 0;
  // What the last checkout of a statement was handed as parameters.
  std::vector<std::size_t> parameter_ordinals;
  std::vector<sql_value> parameters;
};

// Hands back a scripted result set. It fills the buffer bound at index k with
// result column k, which is all a driver can do: it never sees which field the
// caller had in mind, only the positions it was told to bind.
class scripted_statement : public backend::statement_iface {
public:
  explicit scripted_statement(std::shared_ptr<scripted_state> state)
    : state_(std::move(state))
  {}

  void bind_column(
    std::size_t index, backend::column_buffer const& buffer) override {
    if (bound_.size() < index) {
      bound_.resize(index);
    }
    bound_[index - 1] = buffer;
  }

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

  std::vector<column_info> column_meta() const override {
    ++state_->meta_calls;
    return state_->columns;
  }

  void prepare(std::string_view) override {}
  void bind_parameter(std::size_t index, sql_value const& value) override {
    state_->parameter_ordinals.push_back(index);
    state_->parameters.push_back(value);
  }
  void bind_batch_params(std::vector<params> const&) override {}
  void execute() override {}
  std::size_t affected_rows() const override {
    return 0;
  }
  void set_row_array_size(std::size_t size) override {
    row_array_size_ = size;
  }
  std::size_t rows_fetched() const override {
    return fetched_;
  }
  void set_paramset_size(std::size_t) override {}
  backend::batch_writer_iface& prepare_batch() override {
    throw std::logic_error("unused by the read path");
  }
  std::string read_long_text(std::size_t, std::size_t) override {
    throw std::logic_error("unexpected long text read");
  }
  std::vector<std::byte> read_long_bytes(std::size_t, std::size_t) override {
    throw std::logic_error("unexpected long bytes read");
  }
  void reset() override {
    bound_.clear();
    cursor_ = 0;
    fetched_ = 0;
    state_->parameter_ordinals.clear();
    state_->parameters.clear();
  }

private:
  std::shared_ptr<scripted_state> state_;
  std::vector<backend::column_buffer> bound_;
  std::size_t row_array_size_ = 1;
  std::size_t cursor_ = 0;
  std::size_t fetched_ = 0;
};

class scripted_connection : public backend::connection_iface {
public:
  explicit scripted_connection(std::shared_ptr<scripted_state> state)
    : state_(std::move(state))
  {}

  void open(std::string_view) override {}
  void close() override {}
  bool is_open() const noexcept override {
    return true;
  }
  void set_autocommit(bool) override {}
  void commit() override {}
  void rollback() override {}
  backend::capabilities caps() const noexcept override {
    return {};
  }
  std::string dbms_name() const override {
    return "scripted";
  }
  std::unique_ptr<backend::statement_iface> create_statement() override {
    return std::make_unique<scripted_statement>(state_);
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<scripted_state> state_;
};

}  // namespace

void test_binding_order() {
  auto state = std::make_shared<scripted_state>();
  // A driver reports columns in the order the statement asked for them, and a
  // hand-written statement asks in whatever order its author typed.
  state->columns = { { "city", sql_type::varchar, 24, false },
    { "name", sql_type::varchar, 24, false },
    { "id", sql_type::varchar, 24, false } };
  state->rows = { { "Seattle", "Ada", "42" } };

  backend::registry::instance().register_backend(
    "scriptedorder", [state] {
      return std::make_unique<scripted_connection>(state);
    });
  pool_options options;
  options.target = "scriptedorder://test";
  options.size = 1;
  options.acquire_timeout = std::chrono::milliseconds(200);
  options.heartbeat_interval = std::chrono::milliseconds(0);
  connection_pool pool{ options };
  orm database(pool);

  auto mapping = database.map<contact>("contacts");
  mapping.column("id", &contact::id)
    .column("name", &contact::name)
    .column("city", &contact::city);

  // Without a mapping there is no name to consult: field k binds result column
  // k, so this reordered statement transposes the three values and nothing
  // reports it. Every field here is a string, so even the buffer kind matches.
  state->meta_calls = 0;
  auto const positional =
    database.query<unregistered_row>("SELECT city, name, id FROM contacts");
  CHECK(positional.size() == 1);
  CHECK(positional.at(0).id == "Seattle");
  CHECK(positional.at(0).name == "Ada");
  CHECK(positional.at(0).city == "42");
  // Three variable-width fields, one describe round between them.
  CHECK(state->meta_calls == 1);

  // The registered reader of that statement pairs each field with the column
  // carrying its label, and does it for every row of the block.
  state->rows.push_back({ "Portland", "Lin", "7" });
  state->meta_calls = 0;
  state->parameter_ordinals.clear();
  state->parameters.clear();
  auto const paired = database.query<contact>(
    "SELECT city, name, id FROM contacts WHERE city = ?", params("Seattle"));
  CHECK(paired.size() == 2);
  CHECK(paired.at(0).id == "42");
  CHECK(paired.at(0).name == "Ada");
  CHECK(paired.at(0).city == "Seattle");
  CHECK(paired.at(1).id == "7");
  CHECK(paired.at(1).city == "Portland");
  // Three variable-width fields across two fetched rows, one describe round.
  CHECK(state->meta_calls == 1);
  CHECK(state->parameter_ordinals.size() == 1);
  CHECK(state->parameter_ordinals.at(0) == 1);
  CHECK(state->parameters.at(0) == sql_value(std::string("Seattle")));

  // The builder writes its own column list, so its result arrives in
  // registration order and each label is found at its own ordinal.
  state->columns = { { "id", sql_type::varchar, 24, false },
    { "name", sql_type::varchar, 24, false },
    { "city", sql_type::varchar, 24, false } };
  state->rows = { { "42", "Ada", "Seattle" } };
  state->meta_calls = 0;
  auto const built = database.query<contact>().all();
  CHECK(built.size() == 1);
  CHECK(built.at(0).id == "42");
  CHECK(built.at(0).name == "Ada");
  CHECK(built.at(0).city == "Seattle");
  CHECK(state->meta_calls == 1);

  // The dynamic row pairs by label too, which is where the mapping's own
  // lookups come from: same labels, same values.
  std::vector<std::string> const labels{ "city", "name", "id" };
  std::vector<sql_value> const values{ sql_value(std::string("Seattle")),
    sql_value(std::string("Ada")), sql_value(std::string("42")) };
  row named(labels, values);
  CHECK(named.at("id") == sql_value(std::string("42")));
  contact filled{};
  database.meta<contact>().populate(&filled, named);
  CHECK(filled.id == "42");
  CHECK(filled.name == "Ada");
  CHECK(filled.city == "Seattle");

  // A declared name that reaches the server quoted comes back with whatever
  // casing that server uses, so an exact miss still matches a folded label.
  state->columns = { { "NAME", sql_type::varchar, 24, false },
    { "ID", sql_type::varchar, 24, false },
    { "CITY", sql_type::varchar, 24, false } };
  state->rows = { { "Ada", "42", "Seattle" } };
  auto const folded =
    database.query<contact>("SELECT NAME, ID, CITY FROM contacts");
  CHECK(folded.at(0).id == "42");
  CHECK(folded.at(0).city == "Seattle");

  // A column the mapping does not mention costs the read nothing.
  state->columns.push_back({ "note", sql_type::varchar, 24, false });
  state->rows.at(0).push_back("ignored");
  auto const widened =
    database.query<contact>("SELECT NAME, ID, CITY, note FROM contacts");
  CHECK(widened.at(0).id == "42");
  CHECK(widened.at(0).city == "Seattle");

  // A registered class that is no aggregate reads back through its mapping:
  // this is the combination the concept-gated entry point refused outright.
  auto books = database.map<ledger>("books");
  books.column("name", &ledger::name).column("id", &ledger::id);
  state->columns = { { "id", sql_type::varchar, 24, false },
    { "name", sql_type::varchar, 24, false } };
  state->rows = { { "7", "Ada" } };
  auto const read = database.query<ledger>(
    "SELECT id, name FROM books WHERE id = ?", params("7"));
  CHECK(read.size() == 1);
  CHECK(read.at(0).id == "7");
  CHECK(read.at(0).name == "Ada");

  // A mapped column the result does not carry is reported, not guessed at.
  state->columns = { { "name", sql_type::varchar, 24, false },
    { "id", sql_type::varchar, 24, false } };
  state->rows = { { "Ada", "42" } };
  CHECK_THROWS(
    database.query<contact>("SELECT name, id FROM contacts"), column_not_found);

  // A type with neither a mapping nor an aggregate layout has no contract to
  // bind against, and names the two it looked for.
  state->columns = { { "id", sql_type::varchar, 24, false } };
  state->rows = { { "42" } };
  bool refused = false;
  bool reported_both = false;
  try {
    database.query<opaque_row>("SELECT id FROM contacts");
  } catch (mapping_error const& error) {
    refused = true;
    reported_both = std::string_view(error.what()).find(
      "registered entity or an aggregate") != std::string_view::npos;
  }
  CHECK(refused);
  CHECK(reported_both);
}
