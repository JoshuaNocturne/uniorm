#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/error.hpp>
#include <uniorm/params.hpp>
#include <uniorm/value.hpp>

#include "odbc/compat.hpp"
#include "odbc/probe_cache.hpp"

#include "check.hpp"

using namespace uniorm;

namespace {

std::int64_t as_int(sql_value const& value) {
  if (auto const* wide = std::get_if<std::int64_t>(&value)) {
    return *wide;
  }
  if (auto const* word = std::get_if<std::int32_t>(&value)) {
    return *word;
  }
  return 0;
}

std::string as_text(sql_value const& value) {
  if (auto const* text = std::get_if<std::string>(&value)) {
    return *text;
  }
  return {};
}

// A scripted in-memory server the probe core can run on. create_count records
// how many times a probe reached the scratch DDL, so a cache hit is visible as
// the count staying put; create_throws makes the probe inconclusive.
struct cache_model {
  struct row {
    std::int32_t val = 0;
    std::string tag;
  };
  std::map<std::int32_t, row> rows;
  bool open = true;
  bool create_throws = false;
  int create_count = 0;
  std::string dbms = "MySQL";
};

backend::backend_error statement_error(std::string state) {
  return backend::backend_error(
    "fake", "execute", { { std::move(state), 0, "scripted" } });
}

class cache_statement : public backend::statement_iface {
public:
  explicit cache_statement(std::shared_ptr<cache_model> model)
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
    throw std::logic_error(
      "the cache fake never asks for the columnar channel");
  }
  void execute() override {
    if (sql_.rfind("CREATE", 0) == 0) {
      ++model_->create_count;
      if (model_->create_throws) {
        throw statement_error("42000");
      }
      model_->rows.clear();
      return;
    }
    if (sql_.rfind("DROP", 0) == 0) {
      model_->rows.clear();
      return;
    }
    if (sql_.rfind("INSERT", 0) == 0) {
      for (std::size_t index = 0; index < paramset_ && index < batch_->size();
        ++index) {
        params const& row = (*batch_)[index];
        auto const id = static_cast<std::int32_t>(as_int(row.at(0)));
        model_->rows[id] = { static_cast<std::int32_t>(as_int(row.at(1))),
          as_text(row.at(2)) };
      }
      affected_ = paramset_;
      return;
    }
    if (sql_.rfind("UPDATE", 0) == 0) {
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
      affected_ = paramset_;
      return;
    }
    if (sql_.rfind("DELETE", 0) == 0) {
      if (sql_.find("WHERE") == std::string::npos) {
        affected_ = model_->rows.size();
        model_->rows.clear();
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
    throw std::logic_error(
      "the cache fake saw an unexpected statement: " + sql_);
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
  std::shared_ptr<cache_model> model_;
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

class cache_connection : public backend::connection_iface {
public:
  explicit cache_connection(std::shared_ptr<cache_model> model)
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
    return std::make_unique<cache_statement>(model_);
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<cache_model> model_;
};

odbc::version_triple version(
  std::uint32_t major, std::uint32_t minor, std::uint32_t patch) {
  return odbc::version_triple{ major, minor, patch };
}

}  // namespace

void test_odbc_probe_cache() {
  using detail::probe_outcome;

  // The key folds names and carries patch level, so two driver patches that
  // behave differently do not share a verdict.
  {
    std::string const base = odbc::probe_cache_key(
      "maodbc", version(3, 1, 12), "MariaDB", version(10, 6, 0));
    CHECK(base == odbc::probe_cache_key(
                    "MAODBC", version(3, 1, 12), "MariaDB", version(10, 6, 0)));
    CHECK(base != odbc::probe_cache_key(
                    "maodbc", version(3, 1, 23), "MariaDB", version(10, 6, 0)));
    CHECK(base != odbc::probe_cache_key(
                    "myodbc", version(3, 1, 12), "MariaDB", version(10, 6, 0)));
    CHECK(base.find("driver=maodbc") != std::string::npos);
    CHECK(base.find("dver=3.1.12") != std::string::npos);
  }

  // Resolution maps pass/fail to bits and refuses any inconclusive op.
  {
    detail::array_probe_result all_pass;
    all_pass.insert = probe_outcome::pass;
    all_pass.update = probe_outcome::pass;
    all_pass.remove = probe_outcome::pass;
    all_pass.rowcount_totals = false;
    auto resolved = odbc::resolve_probe(all_pass);
    CHECK(resolved.has_value());
    CHECK(resolved->array_insert);
    CHECK(resolved->array_update);
    CHECK(resolved->array_delete);
    CHECK(!resolved->array_rowcount_totals);
  }
  {
    detail::array_probe_result mixed;
    mixed.insert = probe_outcome::pass;
    mixed.update = probe_outcome::fail;
    mixed.remove = probe_outcome::pass;
    auto resolved = odbc::resolve_probe(mixed);
    CHECK(resolved.has_value());
    CHECK(resolved->array_insert);
    CHECK(!resolved->array_update);
    CHECK(resolved->array_delete);
  }
  {
    detail::array_probe_result inconclusive;
    inconclusive.insert = probe_outcome::pass;
    inconclusive.update = probe_outcome::inconclusive;
    inconclusive.remove = probe_outcome::pass;
    CHECK(!odbc::resolve_probe(inconclusive).has_value());
  }

  // A stored verdict is read back; an absent key misses.
  {
    std::string const key = "probe_cache_roundtrip_unique";
    odbc::probe_conclusion stored;
    stored.array_insert = true;
    stored.array_delete = true;
    odbc::probe_cache_store(key, stored);
    odbc::probe_conclusion read;
    CHECK(odbc::probe_cache_lookup(key, read));
    CHECK(read.array_insert);
    CHECK(!read.array_update);
    CHECK(read.array_delete);
    odbc::probe_conclusion absent;
    CHECK(!odbc::probe_cache_lookup("probe_cache_absent_unique", absent));
  }

  // A conclusive probe is applied and cached: the second call reuses it
  // without reaching the scratch DDL again.
  {
    auto model = std::make_shared<cache_model>();
    cache_connection conn(model);
    auto const first = odbc::probe_unknown_combo(
      conn, "probe_pass_drv", version(1, 0, 0), "MySQL", version(8, 0, 0));
    CHECK(first.array_insert);
    CHECK(first.array_update);
    CHECK(first.array_delete);
    CHECK(first.array_rowcount_totals);
    CHECK(model->create_count == 1);
    auto const second = odbc::probe_unknown_combo(
      conn, "probe_pass_drv", version(1, 0, 0), "MySQL", version(8, 0, 0));
    CHECK(second.array_insert);
    CHECK(second.array_update);
    CHECK(second.array_delete);
    CHECK(model->create_count == 1);  // cache hit, no second probe
  }

  // An inconclusive probe (scratch DDL refused) leaves conservative deny and
  // is not cached, so a later connection probes again rather than inheriting a
  // global hard deny that a privileged connection would not deserve.
  {
    auto model = std::make_shared<cache_model>();
    model->create_throws = true;
    cache_connection conn(model);
    auto const first = odbc::probe_unknown_combo(
      conn, "probe_incon_drv", version(1, 0, 0), "MySQL", version(8, 0, 0));
    CHECK(!first.array_insert);
    CHECK(!first.array_update);
    CHECK(!first.array_delete);
    CHECK(first.array_rowcount_totals);
    CHECK(model->create_count == 1);
    auto const second = odbc::probe_unknown_combo(
      conn, "probe_incon_drv", version(1, 0, 0), "MySQL", version(8, 0, 0));
    CHECK(!second.array_insert);
    CHECK(model->create_count == 2);  // not cached: probed again
  }

  // Two connections racing the same unknown combo both reach a consistent
  // verdict; a duplicate first probe is accepted, not serialized away.
  {
    auto run = [](odbc::probe_conclusion& out) {
      auto model = std::make_shared<cache_model>();
      cache_connection conn(model);
      out = odbc::probe_unknown_combo(
        conn, "probe_race_drv", version(1, 0, 0), "MySQL", version(8, 0, 0));
    };
    odbc::probe_conclusion left;
    odbc::probe_conclusion right;
    std::thread first(run, std::ref(left));
    std::thread second(run, std::ref(right));
    first.join();
    second.join();
    CHECK(left.array_insert && left.array_update && left.array_delete);
    CHECK(right.array_insert && right.array_update && right.array_delete);
  }
}