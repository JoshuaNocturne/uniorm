#include "check.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/connection.hpp>
#include <uniorm/result_set.hpp>
#include <uniorm/types.hpp>

using namespace uniorm;

namespace {

// What the two destructors leave behind, in the order they ran. A driver's
// statement frees a handle parented to its connection, so the connection has
// to still be standing when the statement goes.
struct lifetime_state {
  std::vector<std::string> destroyed;
  int magic_seen_at_statement_destroy = 0;
  std::vector<column_info> columns;
};

class lifetime_connection;

// Holds the parent the way a real driver statement does: a raw pointer to the
// connection that made it, read again when the statement is freed.
class lifetime_statement : public backend::statement_iface {
public:
  lifetime_statement(
    std::shared_ptr<lifetime_state> state, lifetime_connection* parent)
    : state_(std::move(state)), parent_(parent) {}

  ~lifetime_statement() override;

  void prepare(std::string_view) override {}
  void bind_parameter(std::size_t, sql_value const&) override {}
  void bind_column(std::size_t, backend::column_buffer const&) override {}
  void bind_batch_params(std::vector<params> const&) override {}
  void execute() override {}
  bool fetch() override {
    return false;
  }
  std::size_t affected_rows() const override {
    return 0;
  }
  std::vector<column_info> column_meta() const override {
    return state_->columns;
  }
  void set_row_array_size(std::size_t) override {}
  std::size_t rows_fetched() const override {
    return 0;
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
  void reset() override {}

private:
  std::shared_ptr<lifetime_state> state_;
  lifetime_connection* parent_;
};

class lifetime_connection : public backend::connection_iface {
public:
  static constexpr int magic = 0x5a5a;

  explicit lifetime_connection(std::shared_ptr<lifetime_state> state)
    : state_(std::move(state)) {}

  ~lifetime_connection() override {
    state_->destroyed.push_back("conn");
  }

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
    return "lifetime";
  }
  std::unique_ptr<backend::statement_iface> create_statement() override {
    return std::make_unique<lifetime_statement>(state_, this);
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<lifetime_state> state_;
};

lifetime_statement::~lifetime_statement() {
  state_->magic_seen_at_statement_destroy = parent_->magic;
  state_->destroyed.push_back("stmt");
}

}  // namespace

// A result_set may outlive the connection that produced it: the statement it
// still holds frees a handle parented to that connection, so the connection
// must not be freed first. The check-in closure keeps the backend alive until
// the result_set is done with it.
void test_result_set_may_outlive_its_connection() {
  auto state = std::make_shared<lifetime_state>();
  state->columns = { { "id", sql_type::varchar, 24, false } };
  backend::registry::instance().register_backend("lifetime",
    [state] { return std::make_unique<lifetime_connection>(state); });

  std::optional<result_set> kept;
  {
    connection conn("lifetime://test");
    kept = conn.execute("SELECT id FROM contacts");
  }  // the connection goes here; the result_set still holds its statement
  CHECK(state->destroyed.empty());

  kept.reset();
  // The statement was freed against a live parent, and only then did the
  // connection go: the other order reads a freed connection.
  CHECK((state->destroyed == std::vector<std::string>{ "stmt", "conn" }));
  CHECK(state->magic_seen_at_statement_destroy == lifetime_connection::magic);
}