#include "check.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/error.hpp>
#include <uniorm/params.hpp>
#include <uniorm/value.hpp>

#include "array_probe.hpp"

using namespace uniorm;

namespace {

std::int64_t as_int(sql_value const& value) {
  if (auto const* wide = std::get_if<std::int64_t>(&value)) {
    return *wide;
  }
  if (auto const* word = std::get_if<std::int32_t>(&value)) {
    return *word;
  }
  if (auto const* half = std::get_if<std::int16_t>(&value)) {
    return *half;
  }
  return 0;
}

std::string as_text(sql_value const& value) {
  if (auto const* text = std::get_if<std::string>(&value)) {
    return *text;
  }
  return {};
}

// The in-memory table the fake statements read and write, plus the script that
// decides which distortion this run reproduces.
struct probe_model {
  struct row {
    std::int32_t val = 0;
    std::string tag;
  };
  std::map<std::int32_t, row> rows;
  bool table_exists = false;
  bool open = true;
  std::string dbms = "MySQL";
  std::string create_error_state;
  bool fail_update = false;
  bool die_on_update = false;
  bool silent_insert = false;
  bool silent_update = false;
  bool silent_delete = false;
  bool short_totals = false;
};

backend::backend_error statement_error(std::string state) {
  return backend::backend_error(
    "fake", "execute", { { std::move(state), 0, "scripted" } });
}

class probe_statement : public backend::statement_iface {
public:
  explicit probe_statement(std::shared_ptr<probe_model> model)
    : model_(std::move(model)) {}

  void prepare(std::string_view sql) override {
    sql_ = std::string(sql);
    batch_ = nullptr;
    paramset_ = 1;
    where_ = sql_value{};
    has_column_ = false;
    has_result_ = false;
    fetched_ = false;
    affected_ = 0;
  }

  void bind_parameter(std::size_t, sql_value const& value) override {
    where_ = value;
  }

  void bind_column(std::size_t, backend::column_buffer const& buffer) override {
    column_ = buffer;
    has_column_ = true;
  }

  void bind_batch_params(std::vector<params> const& rows) override {
    batch_ = &rows;
  }

  backend::batch_writer_iface& prepare_batch() override {
    throw std::logic_error("the probe never asks for the columnar channel");
  }

  void execute() override {
    if (sql_.rfind("CREATE", 0) == 0) {
      if (!model_->create_error_state.empty()) {
        throw statement_error(model_->create_error_state);
      }
      model_->table_exists = true;
      model_->rows.clear();
      return;
    }
    if (sql_.rfind("DROP", 0) == 0) {
      model_->table_exists = false;
      model_->rows.clear();
      return;
    }
    if (sql_.rfind("INSERT", 0) == 0) {
      bool const array = paramset_ > 1;
      if (!(array && model_->silent_insert)) {
        for (std::size_t index = 0; index < paramset_ && index < batch_->size();
          ++index) {
          params const& row = (*batch_)[index];
          auto const id = static_cast<std::int32_t>(as_int(row.at(0)));
          model_->rows[id] = { static_cast<std::int32_t>(as_int(row.at(1))),
            as_text(row.at(2)) };
        }
      }
      affected_ = paramset_;
      return;
    }
    if (sql_.rfind("UPDATE", 0) == 0) {
      if (model_->die_on_update) {
        model_->open = false;
        throw statement_error("08S01");
      }
      if (model_->fail_update) {
        throw statement_error("42000");
      }
      bool const array = paramset_ > 1;
      if (!(array && model_->silent_update)) {
        for (std::size_t index = 0; index < paramset_ && index < batch_->size();
          ++index) {
          params const& row = (*batch_)[index];
          auto const id = static_cast<std::int32_t>(as_int(row.at(2)));
          auto found = model_->rows.find(id);
          if (found != model_->rows.end()) {
            found->second = { static_cast<std::int32_t>(as_int(row.at(0))),
              as_text(row.at(1)) };
          }
        }
      }
      affected_ = array && model_->short_totals ? 1 : paramset_;
      return;
    }
    if (sql_.rfind("DELETE", 0) == 0) {
      if (sql_.find("WHERE") == std::string::npos) {
        affected_ = model_->rows.size();
        model_->rows.clear();
        return;
      }
      bool const array = paramset_ > 1;
      if (array && model_->silent_delete) {
        affected_ = paramset_;
        return;
      }
      for (std::size_t index = 0; index < paramset_ && index < batch_->size();
        ++index) {
        auto const id =
          static_cast<std::int32_t>(as_int((*batch_)[index].at(0)));
        model_->rows.erase(id);
      }
      affected_ = paramset_;
      return;
    }
    if (sql_.rfind("SELECT COUNT", 0) == 0) {
      result_ = static_cast<std::int64_t>(model_->rows.size());
      has_result_ = true;
      return;
    }
    if (sql_.rfind("SELECT val", 0) == 0) {
      auto const id = static_cast<std::int32_t>(as_int(where_));
      auto found = model_->rows.find(id);
      has_result_ = found != model_->rows.end();
      if (has_result_) {
        result_ = found->second.val;
      }
      return;
    }
    throw std::logic_error("the probe issued an unexpected statement: " + sql_);
  }

  bool fetch() override {
    if (fetched_ || !has_result_ || !has_column_) {
      return false;
    }
    fetched_ = true;
    *static_cast<std::int64_t*>(column_.data) = result_;
    *column_.indicator = static_cast<std::int64_t>(sizeof(std::int64_t));
    return true;
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
  void set_paramset_size(std::size_t size) override {
    paramset_ = size;
  }
  std::string read_long_text(std::size_t, std::size_t) override {
    throw std::logic_error("unexpected long text read");
  }
  std::vector<std::byte> read_long_bytes(std::size_t, std::size_t) override {
    throw std::logic_error("unexpected long bytes read");
  }
  void reset() override {
    batch_ = nullptr;
    paramset_ = 1;
    has_result_ = false;
    fetched_ = false;
    affected_ = 0;
  }

private:
  std::shared_ptr<probe_model> model_;
  std::string sql_;
  std::vector<params> const* batch_ = nullptr;
  std::size_t paramset_ = 1;
  sql_value where_;
  backend::column_buffer column_{};
  bool has_column_ = false;
  std::int64_t result_ = 0;
  bool has_result_ = false;
  bool fetched_ = false;
  std::size_t affected_ = 0;
};

class probe_connection : public backend::connection_iface {
public:
  explicit probe_connection(std::shared_ptr<probe_model> model)
    : model_(std::move(model)) {}

  void open(std::string_view) override {}
  void close() override {
    model_->open = false;
  }
  bool is_open() const noexcept override {
    return model_->open;
  }
  void set_autocommit(bool) override {}
  void commit() override {}
  void rollback() override {}
  backend::capabilities caps() const noexcept override {
    return {};
  }
  std::string dbms_name() const override {
    return model_->dbms;
  }
  std::unique_ptr<backend::statement_iface> create_statement() override {
    return std::make_unique<probe_statement>(model_);
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<probe_model> model_;
};

std::shared_ptr<probe_model> run_probe(
  detail::array_probe_result& result, std::size_t sets = 8) {
  auto model = std::make_shared<probe_model>();
  probe_connection conn(model);
  result = detail::probe_array_dml(conn, sets);
  return model;
}

}  // namespace

void test_array_probe() {
  using detail::probe_outcome;

  // A driver that runs every array op and reports the whole array's count.
  {
    detail::array_probe_result result;
    auto model = run_probe(result);
    CHECK(result.insert == probe_outcome::pass);
    CHECK(result.update == probe_outcome::pass);
    CHECK(result.remove == probe_outcome::pass);
    CHECK(result.rowcount_totals);
    CHECK(!model->table_exists);  // the scratch was dropped on the way out
  }

  // A statement-level UPDATE rejection fails that op alone; the connection
  // survives, so DELETE still runs and passes.
  {
    auto model = std::make_shared<probe_model>();
    model->fail_update = true;
    probe_connection conn(model);
    auto result = detail::probe_array_dml(conn);
    CHECK(result.insert == probe_outcome::pass);
    CHECK(result.update == probe_outcome::fail);
    CHECK(result.remove == probe_outcome::pass);
    CHECK(result.rowcount_totals);
  }

  // Success reported, zero rows landed: the SELECT verify catches the no-op
  // that affected_rows() alone would have blessed.
  {
    auto model = std::make_shared<probe_model>();
    model->silent_insert = true;
    probe_connection conn(model);
    auto result = detail::probe_array_dml(conn);
    CHECK(result.insert == probe_outcome::fail);
  }
  {
    auto model = std::make_shared<probe_model>();
    model->silent_update = true;
    probe_connection conn(model);
    auto result = detail::probe_array_dml(conn);
    CHECK(result.update == probe_outcome::fail);
  }
  {
    auto model = std::make_shared<probe_model>();
    model->silent_delete = true;
    probe_connection conn(model);
    auto result = detail::probe_array_dml(conn);
    CHECK(result.remove == probe_outcome::fail);
  }

  // A scratch DDL failure is inconclusive for every op, whether the cause is a
  // lost connection (08) or a missing privilege (42000) -- neither is an op
  // failing to execute.
  {
    auto model = std::make_shared<probe_model>();
    model->create_error_state = "08S01";
    probe_connection conn(model);
    auto result = detail::probe_array_dml(conn);
    CHECK(result.insert == probe_outcome::inconclusive);
    CHECK(result.update == probe_outcome::inconclusive);
    CHECK(result.remove == probe_outcome::inconclusive);
  }
  {
    auto model = std::make_shared<probe_model>();
    model->create_error_state = "42000";
    probe_connection conn(model);
    auto result = detail::probe_array_dml(conn);
    CHECK(result.insert == probe_outcome::inconclusive);
    CHECK(result.update == probe_outcome::inconclusive);
    CHECK(result.remove == probe_outcome::inconclusive);
  }

  // An N-set UPDATE that reports one set's count tallies short: the rows all
  // changed (UPDATE passes) but the count is not the array's.
  {
    auto model = std::make_shared<probe_model>();
    model->short_totals = true;
    probe_connection conn(model);
    auto result = detail::probe_array_dml(conn);
    CHECK(result.update == probe_outcome::pass);
    CHECK(!result.rowcount_totals);
  }

  // A connection that dies mid-UPDATE leaves that op and every op after it
  // inconclusive, not failed.
  {
    auto model = std::make_shared<probe_model>();
    model->die_on_update = true;
    probe_connection conn(model);
    auto result = detail::probe_array_dml(conn);
    CHECK(result.insert == probe_outcome::pass);
    CHECK(result.update == probe_outcome::inconclusive);
    CHECK(result.remove == probe_outcome::inconclusive);
  }

  // A server the dialect does not know takes the permanent unique-name scratch
  // path and still probes clean.
  {
    auto model = std::make_shared<probe_model>();
    model->dbms = "UnknownServer";
    probe_connection conn(model);
    auto result = detail::probe_array_dml(conn);
    CHECK(result.insert == probe_outcome::pass);
    CHECK(result.update == probe_outcome::pass);
    CHECK(result.remove == probe_outcome::pass);
    CHECK(!model->table_exists);
  }

  // A set size below two is clamped up: an array probe needs an array.
  {
    detail::array_probe_result result;
    run_probe(result, 1);
    CHECK(result.insert == probe_outcome::pass);
    CHECK(result.remove == probe_outcome::pass);
  }
}