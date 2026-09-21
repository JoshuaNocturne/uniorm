#include "check.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/error.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/builder/builder.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/pool.hpp>

using namespace uniorm;

namespace {

using parameter_rows = std::vector<std::vector<sql_value>>;

struct account {
  std::int64_t tenant = 0;
  std::int64_t id = 0;
  std::string name;
  std::optional<std::int32_t> balance;
  std::int32_t extra = 0;
  std::int32_t revision = 0;
  std::int32_t scratch = 0;
};

struct event {
  std::int64_t id = 0;
};

struct account_alias {
  std::int64_t id = 0;
};

mapping_builder<account> map_account(
  orm& database, std::string_view table = "ACCOUNTS") {
  auto mapping = database.map<account>(table);
  mapping.primary_key("TENANT_ID", &account::tenant)
    .column("DISPLAY_NAME", &account::name)
    .primary_key("USER_ID", &account::id)
    .column("BALANCE", &account::balance);
  return mapping;
}

bool same_table(schema_meta::table_ref const& left,
  schema_meta::table_ref const& right) {
  return left.catalog == right.catalog && left.schema == right.schema &&
    left.name == right.name;
}

struct catalog_table {
  schema_meta::table_ref ref;
  std::vector<schema_meta::column_row> columns;
};

catalog_table accounts_at(std::string catalog = "Tenant",
  std::string schema = "App", std::string name = "Accounts") {
  return { { std::move(catalog), std::move(schema), std::move(name) }, {
    { { "Balance", sql_type::integer, true }, "integer", 10, 0, {} },
    { { "User_Id", sql_type::bigint, false }, "bigint", 19, 0, {} },
    { { "Display_Name", sql_type::varchar, false }, "varchar", 80, 0, {} },
    { { "Tenant_Id", sql_type::bigint, false }, "bigint", 19, 0, {} },
  } };
}

catalog_table events_at(std::string schema = "App") {
  return { { "Tenant", std::move(schema), "Events" }, {
    { { "Event_Id", sql_type::bigint, false }, "bigint", 19, 0, {} },
  } };
}

class exact_catalog : public schema_meta {
public:
  std::string database = "Tenant";
  std::vector<catalog_table> entries{ accounts_at() };
  std::vector<table_ref> table_reads;
  std::vector<table_ref> column_reads;
  bool fail_tables = false;
  bool fail_columns = false;
  int legacy_reads = 0;

  std::string database_name() override {
    return database;
  }

  std::vector<table_row> exact_tables(
    std::string_view catalog, std::string_view schema) override {
    table_reads.push_back({ std::string(catalog), std::string(schema), {} });
    if (fail_tables) {
      throw mapping_error("injected table listing failure");
    }
    std::vector<table_row> result;
    for (auto const& entry : entries) {
      result.push_back({ entry.ref.catalog, entry.ref.schema, entry.ref.name });
    }
    return result;
  }

  std::vector<column_row> exact_table_columns(table_ref const& ref) override {
    column_reads.push_back(ref);
    if (fail_columns) {
      throw mapping_error("injected column read failure");
    }
    for (auto const& entry : entries) {
      if (same_table(entry.ref, ref)) {
        return entry.columns;
      }
    }
    return {};
  }

  std::vector<table_row> tables(std::string_view, std::string_view) override {
    ++legacy_reads;
    throw std::logic_error("resolution used legacy table listing");
  }

  std::vector<column_row> table_columns(table_ref const&) override {
    ++legacy_reads;
    throw std::logic_error("resolution used legacy column lookup");
  }

  std::vector<std::string> primary_key(table_ref const&) override {
    return {};
  }
  std::vector<foreign_key_row> foreign_keys(table_ref const&) override {
    return {};
  }
  std::vector<index_row> indexes(table_ref const&) override {
    return {};
  }
};

struct execution {
  std::string sql;
  parameter_rows rows;
};

struct recording_state {
  exact_catalog catalog;
  std::string product = "PostgreSQL";
  bool columnar = false;
  bool introspection = true;
  bool fail_open = false;
  std::size_t opens = 0;
  std::size_t row_batches = 0;
  std::size_t column_batches = 0;
  std::vector<std::string> prepared;
  std::vector<execution> executed;
  std::vector<column_info> result_columns;
  parameter_rows result_rows;
};

class recording_batch : public backend::batch_writer_iface {
public:
  std::size_t add_column(backend::buffer_type type, std::size_t count,
    std::size_t element_size) override {
    std::size_t const words =
      (count * element_size + sizeof(std::max_align_t) - 1) /
      sizeof(std::max_align_t);
    columns_.push_back({ type, element_size,
      std::vector<std::max_align_t>(words),
      std::vector<std::int64_t>(count) });
    return columns_.size() - 1;
  }

  void* data(std::size_t column) override {
    return columns_.at(column).storage.data();
  }
  std::size_t element_size(std::size_t column) override {
    return columns_.at(column).stride;
  }
  std::int64_t* indicators(std::size_t column) override {
    return columns_.at(column).indicators.data();
  }
  void finish() override {}

  parameter_rows values(std::size_t count) const {
    parameter_rows result(count);
    for (std::size_t row = 0; row < count; ++row) {
      for (auto const& column : columns_) {
        auto const indicator = column.indicators.at(row);
        auto const* value =
          reinterpret_cast<char const*>(column.storage.data()) +
          row * column.stride;
        if (indicator == backend::null_indicator) {
          result[row].emplace_back(std::monostate{});
        } else if (column.type == backend::buffer_type::int64) {
          std::int64_t number;
          std::memcpy(&number, value, sizeof(number));
          result[row].emplace_back(number);
        } else if (column.type == backend::buffer_type::int32) {
          std::int32_t number;
          std::memcpy(&number, value, sizeof(number));
          result[row].emplace_back(number);
        } else if (column.type == backend::buffer_type::chars) {
          result[row].emplace_back(
            std::string(value, static_cast<std::size_t>(indicator)));
        } else {
          throw std::logic_error("unexpected recorded batch type");
        }
      }
    }
    return result;
  }

private:
  struct column_storage {
    backend::buffer_type type;
    std::size_t stride;
    std::vector<std::max_align_t> storage;
    std::vector<std::int64_t> indicators;
  };
  std::vector<column_storage> columns_;
};

class recording_statement : public backend::statement_iface {
public:
  explicit recording_statement(recording_state& state) : state_(state) {}

  void prepare(std::string_view sql) override {
    sql_ = sql;
    state_.prepared.push_back(sql_);
  }

  void bind_parameter(std::size_t index, sql_value const& value) override {
    if (parameters_.size() < index) {
      parameters_.resize(index);
    }
    parameters_[index - 1] = value;
  }

  void bind_column(
    std::size_t index, backend::column_buffer const& buffer) override {
    if (bound_.size() < index) {
      bound_.resize(index);
    }
    bound_[index - 1] = buffer;
  }

  void bind_batch_params(std::vector<params> const& rows) override {
    ++state_.row_batches;
    rowwise_.clear();
    for (auto const& row : rows) {
      rowwise_.push_back(row.values());
    }
  }

  backend::batch_writer_iface& prepare_batch() override {
    ++state_.column_batches;
    batch_ = std::make_unique<recording_batch>();
    return *batch_;
  }

  void execute() override {
    parameter_rows rows;
    if (batch_) {
      rows = batch_->values(paramset_size_);
    } else if (!rowwise_.empty()) {
      rows = rowwise_;
    } else {
      rows.push_back(parameters_);
    }
    state_.executed.push_back({ sql_, std::move(rows) });
    cursor_ = 0;
  }

  bool fetch() override {
    fetched_ = std::min(row_array_size_, state_.result_rows.size() - cursor_);
    for (std::size_t row = 0; row < fetched_; ++row) {
      auto const& values = state_.result_rows.at(cursor_ + row);
      for (std::size_t column = 0; column < bound_.size(); ++column) {
        auto const& value = values.at(column);
        auto const& buffer = bound_[column];
        auto* target = static_cast<char*>(buffer.data) + row * buffer.capacity;
        if (std::holds_alternative<std::monostate>(value)) {
          buffer.indicator[row] = backend::null_indicator;
        } else if (auto number = std::get_if<std::int64_t>(&value)) {
          CHECK(buffer.type == backend::buffer_type::int64);
          std::memcpy(target, number, sizeof(*number));
          buffer.indicator[row] = sizeof(*number);
        } else if (auto number = std::get_if<std::int32_t>(&value)) {
          CHECK(buffer.type == backend::buffer_type::int32);
          std::memcpy(target, number, sizeof(*number));
          buffer.indicator[row] = sizeof(*number);
        } else if (auto text = std::get_if<std::string>(&value)) {
          CHECK(buffer.type == backend::buffer_type::chars);
          if (text->size() >= buffer.capacity) {
            throw std::logic_error("recorded text exceeds fetch buffer");
          }
          std::memcpy(target, text->data(), text->size());
          buffer.indicator[row] = static_cast<std::int64_t>(text->size());
        } else {
          throw std::logic_error("unexpected recorded result type");
        }
      }
    }
    cursor_ += fetched_;
    return fetched_ != 0;
  }

  std::size_t affected_rows() const override {
    return paramset_size_;
  }
  std::vector<column_info> column_meta() const override {
    return state_.result_columns;
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
  std::string read_long_text(std::size_t) override {
    throw std::logic_error("unexpected long text read");
  }
  std::vector<std::byte> read_long_bytes(std::size_t) override {
    throw std::logic_error("unexpected long bytes read");
  }
  void reset() override {
    parameters_.clear();
    rowwise_.clear();
    batch_.reset();
    bound_.clear();
    paramset_size_ = 1;
    cursor_ = 0;
    fetched_ = 0;
  }

private:
  recording_state& state_;
  std::string sql_;
  std::vector<sql_value> parameters_;
  parameter_rows rowwise_;
  std::unique_ptr<recording_batch> batch_;
  std::vector<backend::column_buffer> bound_;
  std::size_t paramset_size_ = 1;
  std::size_t row_array_size_ = 1;
  std::size_t cursor_ = 0;
  std::size_t fetched_ = 0;
};

class recording_connection : public backend::connection_iface {
public:
  explicit recording_connection(std::shared_ptr<recording_state> state)
    : state_(std::move(state)) {}

  void open(std::string_view) override {
    if (state_->fail_open) {
      throw uniorm_error("injected connection failure");
    }
    ++state_->opens;
    open_ = true;
  }
  void close() override {
    open_ = false;
  }
  bool is_open() const noexcept override {
    return open_;
  }
  void set_autocommit(bool) override {}
  void commit() override {}
  void rollback() override {}
  backend::capabilities caps() const noexcept override {
    return { state_->columnar, true };
  }
  std::string dbms_name() const override {
    return state_->product;
  }
  std::unique_ptr<backend::statement_iface> create_statement() override {
    return std::make_unique<recording_statement>(*state_);
  }
  schema_meta* schema() noexcept override {
    return state_->introspection ? &state_->catalog : nullptr;
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<recording_state> state_;
  bool open_ = false;
};

pool_options register_recording_backend(
  std::shared_ptr<recording_state> const& state) {
  static std::size_t next_backend = 0;
  std::string const scheme = "identifierfake" + std::to_string(next_backend++);
  backend::registry::instance().register_backend(scheme, [state] {
    return std::make_unique<recording_connection>(state);
  });
  pool_options options;
  options.connection_string = scheme + "://test;DSN=" + scheme;
  options.size = 1;
  options.acquire_timeout = std::chrono::milliseconds(200);
  options.heartbeat_interval = std::chrono::milliseconds(0);
  return options;
}

struct recording_fixture {
  std::shared_ptr<recording_state> state = std::make_shared<recording_state>();
  pool_options options = register_recording_backend(state);
  connection_pool pool{ options };
};

template <class F>
std::string mapping_failure(F&& operation) {
  try {
    operation();
  } catch (mapping_error const& error) {
    return error.what();
  }
  CHECK(false);
  return {};
}

void check_resolution(entity_meta const& metadata,
  schema_meta::table_ref const& table,
  std::vector<std::string> const& columns) {
  CHECK(metadata.resolved.has_value());
  if (!metadata.resolved) {
    return;
  }
  CHECK(same_table(metadata.resolved->table, table));
  CHECK(metadata.resolved->columns == columns);
}

void check_declarations(entity_meta const& metadata) {
  CHECK(metadata.table == "ACCOUNTS");
  CHECK(metadata.columns.size() == 4);
  CHECK(metadata.columns.at(0).column == "TENANT_ID");
  CHECK(metadata.columns.at(1).column == "DISPLAY_NAME");
  CHECK(metadata.columns.at(2).column == "USER_ID");
  CHECK(metadata.columns.at(3).column == "BALANCE");
  CHECK(metadata.column_name(make_member_key(&account::name)) ==
    "DISPLAY_NAME");
}

void check_executions(recording_state& state, std::string const& sql,
  parameter_rows const& expected) {
  CHECK(!state.executed.empty());
  CHECK(std::find(state.prepared.begin(), state.prepared.end(), sql) !=
    state.prepared.end());
  parameter_rows rows;
  for (auto const& execution : state.executed) {
    CHECK(execution.sql == sql);
    rows.insert(rows.end(), execution.rows.begin(), execution.rows.end());
  }
  CHECK(rows == expected);
  state.executed.clear();
}

void test_exact_scope_and_order() {
  recording_fixture fixture;
  auto& catalog = fixture.state->catalog;
  catalog.entries = { accounts_at("Tenant", "Elsewhere", "ACCOUNTS"),
    accounts_at("Other", "App", "ACCOUNTS"),
    accounts_at("tenant", "App", "ACCOUNTS"),
    accounts_at("Tenant", "app", "ACCOUNTS"), accounts_at(), accounts_at() };
  orm database(fixture.pool);
  map_account(database);
  database.map<account_alias>("ACCOUNTS")
    .primary_key("USER_ID", &account_alias::id);
  database.resolve_identifiers("Tenant", "App");
  check_resolution(database.meta<account>(), { "Tenant", "App", "Accounts" },
    { "Tenant_Id", "Display_Name", "User_Id", "Balance" });
  check_declarations(database.meta<account>());
  CHECK(catalog.table_reads.size() == 1);
  CHECK(same_table(catalog.table_reads.at(0), { "Tenant", "App", {} }));
  CHECK(catalog.column_reads.size() == 1);
  CHECK(same_table(catalog.column_reads.at(0),
    { "Tenant", "App", "Accounts" }));
  CHECK(catalog.legacy_reads == 0);

  auto exact = accounts_at("Tenant", "App", "ACCOUNTS");
  exact.columns.push_back(
    { { "USER_ID", sql_type::bigint, false }, "bigint", 19, 0, {} });
  catalog.entries.push_back(exact);
  database.resolve_identifiers("Tenant", "App");
  check_resolution(database.meta<account>(), { "Tenant", "App", "ACCOUNTS" },
    { "Tenant_Id", "Display_Name", "USER_ID", "Balance" });
  CHECK(catalog.table_reads.size() == 2);
  CHECK(catalog.column_reads.size() == 2);
  std::reverse(catalog.entries.begin(), catalog.entries.end());
  for (auto& entry : catalog.entries) {
    std::reverse(entry.columns.begin(), entry.columns.end());
  }
  database.resolve_identifiers("Tenant", "App");
  check_resolution(database.meta<account>(), { "Tenant", "App", "ACCOUNTS" },
    { "Tenant_Id", "Display_Name", "USER_ID", "Balance" });
  CHECK(catalog.table_reads.size() == 3);
  CHECK(catalog.column_reads.size() == 3);
  check_declarations(database.meta<account>());
}

void test_resolution_failures_are_atomic() {
  recording_fixture fixture;
  auto& catalog = fixture.state->catalog;
  catalog.entries.push_back(events_at());
  orm database(fixture.pool);
  map_account(database);
  database.map<event>("EVENTS").primary_key("EVENT_ID", &event::id);
  auto const* account_address = &database.meta<account>();
  auto const* event_address = &database.meta<event>();
  auto const* columns_address = account_address->columns.data();
  auto gateway = database.query();
  auto retained = gateway.of<account>();
  database.resolve_identifiers("Tenant", "App");
  CHECK(account_address->columns.data() == columns_address);
  std::string const previous_sql = retained.build_select();
  auto check_previous = [&] {
    CHECK(&database.meta<account>() == account_address);
    CHECK(&database.meta<event>() == event_address);
    CHECK(account_address->columns.data() == columns_address);
    check_resolution(*account_address, { "Tenant", "App", "Accounts" },
      { "Tenant_Id", "Display_Name", "User_Id", "Balance" });
    check_resolution(*event_address, { "Tenant", "App", "Events" },
      { "Event_Id" });
    CHECK(retained.build_select() == previous_sql);
    check_declarations(*account_address);
  };

  catalog.entries.push_back(accounts_at("Tenant", "Next", "ACCOUNTS"));
  CHECK_THROWS(database.resolve_identifiers("Tenant", "Next"), mapping_error);
  check_previous();
  catalog.entries.back() = events_at("Next");
  CHECK_THROWS(database.resolve_identifiers("Tenant", "Next"), mapping_error);
  check_previous();
  catalog.entries.back() = accounts_at("Tenant", "Next", "ACCOUNTS");
  catalog.entries.push_back(events_at("Next"));
  catalog.entries.back().columns.clear();
  CHECK_THROWS(database.resolve_identifiers("Tenant", "Next"), mapping_error);
  check_previous();
  catalog.entries.back() = events_at("Next");
  catalog.fail_tables = true;
  CHECK_THROWS(database.resolve_identifiers("Tenant", "Next"), mapping_error);
  check_previous();
  catalog.fail_tables = false;
  catalog.fail_columns = true;
  CHECK_THROWS(database.resolve_identifiers("Tenant", "Next"), mapping_error);
  check_previous();
  catalog.fail_columns = false;

  database.resolve_identifiers("Tenant", "Next");
  CHECK(&database.meta<account>() == account_address);
  CHECK(&database.meta<event>() == event_address);
  CHECK(account_address->columns.data() == columns_address);
  check_resolution(*account_address, { "Tenant", "Next", "ACCOUNTS" },
    { "Tenant_Id", "Display_Name", "User_Id", "Balance" });
  CHECK(retained.build_select() ==
    "SELECT \"Tenant_Id\", \"Display_Name\", \"User_Id\", \"Balance\" "
    "FROM \"Next\".\"ACCOUNTS\"");
  check_declarations(*account_address);
}

void test_ambiguous_missing_and_conflicting_catalogs() {
  recording_fixture fixture;
  auto& catalog = fixture.state->catalog;
  orm database(fixture.pool);
  map_account(database);
  auto rejected = [&] {
    auto message = mapping_failure([&] {
      database.resolve_identifiers("Tenant", "App");
    });
    CHECK(!database.meta<account>().resolved);
    return message;
  };

  catalog.entries = { accounts_at("Tenant", "App", "Accounts"),
    accounts_at("Tenant", "App", "accounts") };
  auto const ambiguous = rejected();
  CHECK(ambiguous.find("ACCOUNTS") != std::string::npos);
  CHECK(ambiguous.find("Accounts, accounts") != std::string::npos);
  CHECK(ambiguous.find("Tenant.App") != std::string::npos);
  std::reverse(catalog.entries.begin(), catalog.entries.end());
  CHECK(rejected() == ambiguous);

  catalog.entries = { accounts_at("Tenant", "Elsewhere", "ACCOUNTS") };
  CHECK(rejected().find("table not found") != std::string::npos);
  CHECK(catalog.column_reads.empty());
  catalog.entries = { accounts_at() };
  catalog.entries[0].columns.erase(catalog.entries[0].columns.begin() + 1);
  CHECK(rejected().find("USER_ID") != std::string::npos);

  catalog.entries = { accounts_at() };
  auto alternate = catalog.entries[0].columns[1];
  alternate.shape.name = "user_id";
  catalog.entries[0].columns.push_back(alternate);
  auto const ambiguous_column = rejected();
  CHECK(ambiguous_column.find("User_Id, user_id") != std::string::npos);
  std::reverse(catalog.entries[0].columns.begin(),
    catalog.entries[0].columns.end());
  CHECK(rejected() == ambiguous_column);

  for (int conflict = 0; conflict < 6; ++conflict) {
    catalog.entries = { accounts_at() };
    auto duplicate = catalog.entries[0].columns[1];
    switch (conflict) {
    case 0: duplicate.shape.type = sql_type::integer; break;
    case 1: duplicate.shape.nullable = true; break;
    case 2: duplicate.type_name = "int8"; break;
    case 3: duplicate.size = 20; break;
    case 4: duplicate.decimals = 1; break;
    case 5: duplicate.default_value = "0"; break;
    }
    catalog.entries[0].columns.push_back(duplicate);
    auto const conflicting = rejected();
    CHECK(conflicting.find("conflicting catalog column") != std::string::npos);
    CHECK(conflicting.find("User_Id") != std::string::npos);
    std::reverse(catalog.entries[0].columns.begin(),
      catalog.entries[0].columns.end());
    CHECK(rejected() == conflicting);
  }

  catalog.entries = { accounts_at() };
  catalog.entries[0].columns.push_back(catalog.entries[0].columns[1]);
  database.resolve_identifiers("Tenant", "App");
  check_resolution(database.meta<account>(), { "Tenant", "App", "Accounts" },
    { "Tenant_Id", "Display_Name", "User_Id", "Balance" });

  recording_fixture duplicate_fixture;
  orm duplicate_mapping(duplicate_fixture.pool);
  duplicate_mapping.map<account>("ACCOUNTS")
    .column("USER_ID", &account::tenant)
    .column("user_id", &account::id);
  auto const duplicate_destination = mapping_failure([&] {
    duplicate_mapping.resolve_identifiers("Tenant", "App");
  });
  CHECK(duplicate_destination.find("multiple mapped columns resolve to") !=
    std::string::npos);
  CHECK(duplicate_destination.find("User_Id") != std::string::npos);
  CHECK(!duplicate_mapping.meta<account>().resolved);
}

void test_new_registration_and_retained_builders() {
  recording_fixture fixture;
  orm database(fixture.pool);
  auto mapping = map_account(database);
  auto const* address = &database.meta<account>();
  auto gateway = database.query();
  auto retained = gateway.of<account>();
  CHECK(retained.build_select() ==
    "SELECT \"TENANT_ID\", \"DISPLAY_NAME\", \"USER_ID\", \"BALANCE\" "
    "FROM \"ACCOUNTS\"");
  database.resolve_identifiers("Tenant", "App");
  CHECK_THROWS(mapping.column("EXTRA", &account::extra), mapping_error);
  CHECK_THROWS(mapping.primary_key("REVISION", &account::revision),
    mapping_error);
  CHECK_THROWS(mapping.ignore(&account::scratch), mapping_error);
  check_declarations(*address);
  CHECK(address->ignored.empty());

  database.map<event>("EVENTS").primary_key("EVENT_ID", &event::id);
  CHECK(!database.meta<event>().resolved);
  CHECK_THROWS(database.resolve_identifiers("Tenant", "App"), mapping_error);
  CHECK(!database.meta<event>().resolved);
  CHECK(database.meta<account>().resolved.has_value());
  CHECK(&database.meta<account>() == address);
  CHECK_THROWS(mapping.ignore(&account::scratch), mapping_error);
  CHECK(retained.build_select().find("\"App\".\"Accounts\"") !=
    std::string::npos);
  fixture.state->catalog.entries.push_back(events_at());
  database.resolve_identifiers("Tenant", "App");
  check_resolution(database.meta<event>(),
    { "Tenant", "App", "Events" }, { "Event_Id" });

  database.clear_identifier_resolution();
  database.clear_identifier_resolution();
  CHECK(!address->resolved);
  CHECK(!database.meta<event>().resolved);
  CHECK(&database.meta<account>() == address);
  CHECK(retained.build_select() ==
    "SELECT \"TENANT_ID\", \"DISPLAY_NAME\", \"USER_ID\", \"BALANCE\" "
    "FROM \"ACCOUNTS\"");
  mapping.column("EXTRA", &account::extra)
    .primary_key("REVISION", &account::revision)
    .ignore(&account::scratch);
  CHECK(address->columns.size() == 6);
  CHECK(address->ignored.size() == 1);
}

void test_resolved_validation_uses_exact_names() {
  recording_fixture fixture;
  auto& catalog = fixture.state->catalog;
  catalog.entries[0].columns[1].shape.type = sql_type::date;
  catalog.entries[0].columns[2].shape.nullable = true;
  orm database(fixture.pool);
  map_account(database);
  database.identifier_case(dialect::identifier_case::lower);
  database.resolve_identifiers("Tenant", "App");
  catalog.entries[0].columns[1].shape.type = sql_type::bigint;
  catalog.entries[0].columns[2].shape.nullable = false;
  auto const* address = &database.meta<account>();
  auto const table_reads = catalog.table_reads.size();
  for (auto policy : { dialect::identifier_case::lower,
         dialect::identifier_case::upper }) {
    database.identifier_case(policy);
    database.validate(validation_mode::strict);
    CHECK(address->table_sql(database.native_connection().sql_dialect()) ==
      "\"App\".\"Accounts\"");
    CHECK(address->column_sql(make_member_key(&account::name),
      database.native_connection().sql_dialect()) == "\"Display_Name\"");
  }
  database.native_connection().identifier_case(dialect::identifier_case::lower);
  database.validate();
  CHECK(catalog.table_reads.size() == table_reads);
  CHECK(catalog.legacy_reads == 0);

  catalog.entries[0].columns[1].shape.type = sql_type::date;
  CHECK_THROWS(database.validate(validation_mode::strict), mapping_error);
  database.validate(validation_mode::lenient);
  catalog.entries[0].columns[1].shape.type = sql_type::bigint;
  catalog.entries[0].columns[2].shape.nullable = true;
  CHECK_THROWS(database.validate(validation_mode::strict), mapping_error);
  database.validate(validation_mode::lenient);
  catalog.entries[0].columns[2].shape.nullable = false;
  catalog.entries[0].columns[1].shape.name = "USER_ID";
  auto const renamed = mapping_failure([&] { database.validate(); });
  CHECK(renamed.find("User_Id") != std::string::npos);
  check_resolution(*address, { "Tenant", "App", "Accounts" },
    { "Tenant_Id", "Display_Name", "User_Id", "Balance" });
  CHECK(catalog.table_reads.size() == table_reads);
  for (auto const& read : catalog.column_reads) {
    CHECK(same_table(read, { "Tenant", "App", "Accounts" }));
  }
}

void test_non_ascii_names_are_byte_preserving() {
  recording_fixture fixture;
  auto& catalog = fixture.state->catalog;
  catalog.entries = { accounts_at("Tenant", "App", "\xc3\x84_Table") };
  catalog.entries[0].columns[1].shape.name = "\xc3\x84_Id";
  orm database(fixture.pool);
  database.map<event>("\xc3\x84_TABLE")
    .primary_key("\xc3\x84_ID", &event::id);
  database.resolve_identifiers("Tenant", "App");
  check_resolution(database.meta<event>(),
    { "Tenant", "App", "\xc3\x84_Table" }, { "\xc3\x84_Id" });
  auto gateway = database.query();
  auto query = gateway.of<event>();
  CHECK(query.build_select() ==
    "SELECT \"\xc3\x84_Id\" FROM \"App\".\"\xc3\x84_Table\"");
  catalog.entries[0].columns[1].shape.name = "\xc3\xa4_Id";
  CHECK_THROWS(database.resolve_identifiers("Tenant", "App"), mapping_error);
  check_resolution(database.meta<event>(),
    { "Tenant", "App", "\xc3\x84_Table" }, { "\xc3\x84_Id" });
  database.clear_identifier_resolution();
  catalog.entries[0].ref.name = "\xc3\xa4_Table";
  CHECK_THROWS(database.resolve_identifiers("Tenant", "App"), mapping_error);
  CHECK(!database.meta<event>().resolved);
}

void test_scope_requirements_and_unsupported_backends() {
  static_assert(noexcept(std::declval<orm&>().clear_identifier_resolution()));
  orm disconnected;
  disconnected.clear_identifier_resolution();
  map_account(disconnected);
  CHECK_THROWS(disconnected.resolve_identifiers("Tenant", "App"),
    uniorm_error);

  recording_fixture fixture;
  orm database(fixture.pool);
  map_account(database);
  CHECK_THROWS(database.resolve_identifiers({}, "App"), mapping_error);
  CHECK_THROWS(database.resolve_identifiers("Tenant", {}), mapping_error);
  CHECK_THROWS(database.resolve_identifiers("Other", "App"), mapping_error);
  CHECK_THROWS(database.resolve_identifiers("tenant", "App"), mapping_error);
  std::string const nul_catalog("Tenant\0Other", 12);
  std::string const nul_schema("App\0Other", 9);
  CHECK_THROWS(database.resolve_identifiers(nul_catalog, "App"), mapping_error);
  CHECK_THROWS(database.resolve_identifiers("Tenant", nul_schema),
    mapping_error);
  CHECK(fixture.state->catalog.table_reads.empty());
  fixture.state->introspection = false;
  CHECK_THROWS(database.resolve_identifiers("Tenant", "App"),
    backend::capability_not_supported);
  CHECK(!database.meta<account>().resolved);

  recording_fixture unsupported;
  unsupported.state->product = "unknown database";
  orm unknown(unsupported.pool);
  map_account(unknown);
  CHECK_THROWS(unknown.resolve_identifiers("Tenant", "App"),
    backend::capability_not_supported);
  CHECK(unsupported.state->catalog.table_reads.empty());
  CHECK(!unknown.meta<account>().resolved);
}

void test_lifecycle_and_pool_reuse() {
  recording_fixture fixture;
  orm database(fixture.pool);
  map_account(database);
  database.identifier_case(dialect::identifier_case::lower);
  database.resolve_identifiers("Tenant", "App");
  auto const* address = &database.meta<account>();
  orm moved(std::move(database));
  CHECK(&moved.meta<account>() == address);
  CHECK(moved.identifier_case() == dialect::identifier_case::lower);
  check_resolution(moved.meta<account>(), { "Tenant", "App", "Accounts" },
    { "Tenant_Id", "Display_Name", "User_Id", "Balance" });

  recording_fixture replacement;
  orm assigned(replacement.pool);
  assigned.map<event>("ACCOUNTS").primary_key("USER_ID", &event::id);
  assigned.resolve_identifiers("Tenant", "App");
  assigned = std::move(moved);
  CHECK(assigned.size() == 1);
  CHECK(&assigned.meta<account>() == address);
  CHECK(assigned.meta<account>().resolved.has_value());
  CHECK(replacement.pool.idle_count() == 1);
  assigned.disconnect();
  CHECK(!assigned.meta<account>().resolved);
  check_declarations(assigned.meta<account>());
  CHECK_THROWS(assigned.native_connection(), uniorm_error);
  CHECK_THROWS(assigned.resolve_identifiers("Tenant", "App"), uniorm_error);
  CHECK(fixture.pool.idle_count() == 1);

  {
    orm reused(fixture.pool);
    map_account(reused);
    CHECK(fixture.state->opens == 1);
    CHECK(!reused.meta<account>().resolved);
    auto gateway = reused.query();
    auto query = gateway.of<account>();
    CHECK(query.build_select() ==
      "SELECT \"TENANT_ID\", \"DISPLAY_NAME\", \"USER_ID\", \"BALANCE\" "
      "FROM \"ACCOUNTS\"");
    reused.resolve_identifiers("Tenant", "App");
  }
  CHECK(fixture.pool.idle_count() == 1);

  recording_fixture failed;
  failed.state->fail_open = true;
  connection_pool_registry::instance().configure(
    failed.options.connection_string, failed.options);
  connection_pool_registry::instance().configure(
    replacement.options.connection_string, replacement.options);
  orm reconnecting(fixture.pool);
  map_account(reconnecting);
  reconnecting.resolve_identifiers("Tenant", "App");
  CHECK_THROWS(reconnecting.connect(failed.options.connection_string),
    uniorm_error);
  CHECK(!reconnecting.meta<account>().resolved);
  check_declarations(reconnecting.meta<account>());
  reconnecting.resolve_identifiers("Tenant", "App");
  reconnecting.connect(replacement.options.connection_string);
  CHECK(!reconnecting.meta<account>().resolved);
  check_declarations(reconnecting.meta<account>());
  reconnecting.resolve_identifiers("Tenant", "App");
  CHECK(reconnecting.meta<account>().resolved.has_value());
  reconnecting.disconnect();
  CHECK(!reconnecting.meta<account>().resolved);
}

void test_queries_and_statement_cache() {
  recording_fixture fixture;
  auto& state = *fixture.state;
  orm database(fixture.pool);
  map_account(database);
  database.row_array_size(1);
  auto gateway = database.query();
  auto retained = gateway.of<account>();
  retained.where(eq(&account::id, std::int64_t{ 17 }))
    .order_by(&account::name, direction::desc);
  database.resolve_identifiers("Tenant", "App");
  std::string const select =
    "SELECT \"Tenant_Id\", \"Display_Name\", \"User_Id\", \"Balance\" "
    "FROM \"App\".\"Accounts\" WHERE \"User_Id\" = ? "
    "ORDER BY \"Display_Name\" DESC";
  CHECK(retained.build_select() == select);
  state.result_columns = { { "Tenant_Id", sql_type::bigint, 19, false },
    { "Display_Name", sql_type::varchar, 80, false },
    { "User_Id", sql_type::bigint, 19, false },
    { "Balance", sql_type::integer, 10, true } };
  state.result_rows = { params(std::int64_t{ 8 }, "Ada", std::int64_t{ 17 },
    std::int32_t{ 25 }).values(),
    params(std::int64_t{ 9 }, "Lin", std::int64_t{ 17 }, nullptr).values() };
  auto rows = retained.all();
  CHECK(rows.size() == 2);
  CHECK(rows.at(0).tenant == 8);
  CHECK(rows.at(0).id == 17);
  CHECK(rows.at(0).name == "Ada");
  CHECK(rows.at(0).balance == 25);
  CHECK(rows.at(1).tenant == 9);
  CHECK(rows.at(1).name == "Lin");
  CHECK(!rows.at(1).balance);
  parameter_rows const predicate_params{ params(std::int64_t{ 17 }).values() };
  check_executions(state, select, predicate_params);

  auto const prepared_count = state.prepared.size();
  database.identifier_case(dialect::identifier_case::upper);
  database.native_connection().identifier_case(dialect::identifier_case::lower);
  CHECK(retained.all().size() == 2);
  check_executions(state, select, predicate_params);
  CHECK(state.prepared.size() == prepared_count);
  CHECK(database.statement_cache_hits() > 0);
  auto one = retained.one();
  CHECK(one.has_value());
  CHECK(one && one->id == 17 && one->name == "Ada");
  check_executions(state, select + " OFFSET 0 ROWS FETCH NEXT 1 ROWS ONLY",
    predicate_params);

  state.result_columns = { { "count", sql_type::bigint, 19, false } };
  state.result_rows = { params(std::int64_t{ 2 }).values() };
  CHECK(retained.count() == 2);
  check_executions(state,
    "SELECT COUNT(*) FROM \"App\".\"Accounts\" WHERE \"User_Id\" = ?",
    predicate_params);
  state.result_rows.clear();
  CHECK(!retained.one());
  check_executions(state, select + " OFFSET 0 ROWS FETCH NEXT 1 ROWS ONLY",
    predicate_params);

  auto update = gateway.of<account>();
  CHECK(update.set(&account::name, "Grace")
    .set(&account::balance, nullptr)
    .where(eq(&account::tenant, std::int64_t{ 8 }))
    .where(eq(&account::id, std::int64_t{ 17 })).update() == 1);
  check_executions(state,
    "UPDATE \"App\".\"Accounts\" SET \"Display_Name\" = ?, \"Balance\" = ? "
    "WHERE \"Tenant_Id\" = ? AND \"User_Id\" = ?",
    { params("Grace", nullptr,
      std::int64_t{ 8 }, std::int64_t{ 17 }).values() });
  auto remove = gateway.of<account>();
  CHECK(remove.where(eq(&account::id, std::int64_t{ 17 })).remove() == 1);
  check_executions(state,
    "DELETE FROM \"App\".\"Accounts\" WHERE \"User_Id\" = ?", predicate_params);

  database.clear_identifier_resolution();
  auto const declared =
    "SELECT \"tenant_id\", \"display_name\", \"user_id\", \"balance\" "
    "FROM \"accounts\" WHERE \"user_id\" = ? "
    "ORDER BY \"display_name\" DESC";
  CHECK(retained.all().empty());
  check_executions(state, declared, predicate_params);
  CHECK(state.catalog.table_reads.size() == 1);
}

void test_direct_crud(bool columnar) {
  recording_fixture fixture;
  auto& state = *fixture.state;
  state.columnar = columnar;
  orm database(fixture.pool);
  map_account(database);
  database.paramset_size(2);
  database.identifier_case(dialect::identifier_case::lower);
  database.resolve_identifiers("Tenant", "App");
  database.identifier_case(dialect::identifier_case::upper);
  std::vector<account> const rows{ { 8, 17, "Ada", 25 },
    { 9, 18, "Lin", std::nullopt }, { 10, 19, "Grace", 50 } };
  std::string const insert_sql =
    "INSERT INTO \"App\".\"Accounts\" "
    "(\"Tenant_Id\", \"Display_Name\", \"User_Id\", \"Balance\") "
    "VALUES (?, ?, ?, ?)";
  std::string const update_sql =
    "UPDATE \"App\".\"Accounts\" SET \"Display_Name\" = ?, \"Balance\" = ? "
    "WHERE \"Tenant_Id\" = ? AND \"User_Id\" = ?";
  std::string const remove_sql =
    "DELETE FROM \"App\".\"Accounts\" "
    "WHERE \"Tenant_Id\" = ? AND \"User_Id\" = ?";
  parameter_rows insert_params;
  parameter_rows update_params;
  parameter_rows remove_params;
  for (auto const& row : rows) {
    sql_value const balance = row.balance
      ? sql_value(*row.balance) : sql_value(std::monostate{});
    insert_params.push_back(params(row.tenant, row.name, row.id).values());
    insert_params.back().push_back(balance);
    update_params.push_back({ row.name, balance, row.tenant, row.id });
    remove_params.push_back(params(row.tenant, row.id).values());
  }
  CHECK(database.insert(std::vector<account>{ rows.front() }) == 1);
  check_executions(state, insert_sql, { insert_params.front() });
  CHECK(database.insert(rows) == 3);
  CHECK(state.executed.size() == 2);
  check_executions(state, insert_sql, insert_params);
  CHECK(database.update(rows.front()) == 1);
  check_executions(state, update_sql, { update_params.front() });
  CHECK(database.update(rows) == 3);
  CHECK(state.executed.size() == 2);
  check_executions(state, update_sql, update_params);
  CHECK(database.remove(rows.front()) == 1);
  check_executions(state, remove_sql, { remove_params.front() });
  CHECK(database.remove(rows) == 3);
  CHECK(state.executed.size() == 2);
  check_executions(state, remove_sql, remove_params);

  std::vector<std::string> const where{ "USER_ID", "TENANT_ID" };
  std::string const explicit_update =
    "UPDATE \"App\".\"Accounts\" SET \"Display_Name\" = ?, \"Balance\" = ? "
    "WHERE \"User_Id\" = ? AND \"Tenant_Id\" = ?";
  std::string const explicit_remove =
    "DELETE FROM \"App\".\"Accounts\" "
    "WHERE \"User_Id\" = ? AND \"Tenant_Id\" = ?";
  for (auto& parameters : update_params) {
    std::swap(parameters[2], parameters[3]);
  }
  for (auto& parameters : remove_params) {
    std::swap(parameters[0], parameters[1]);
  }
  CHECK(database.update(rows.front(), where) == 1);
  check_executions(state, explicit_update, { update_params.front() });
  CHECK(database.update(rows, where) == 3);
  check_executions(state, explicit_update, update_params);
  CHECK(database.remove(rows.front(), where) == 1);
  check_executions(state, explicit_remove, { remove_params.front() });
  CHECK(database.remove(rows, where) == 3);
  check_executions(state, explicit_remove, remove_params);

  parameter_rows named_update;
  parameter_rows named_remove;
  for (auto const& row : rows) {
    sql_value const balance = row.balance
      ? sql_value(*row.balance) : sql_value(std::monostate{});
    named_update.push_back({ row.tenant, row.id, balance, row.name });
    named_remove.push_back(params(row.name).values());
  }
  CHECK(database.update(rows, { "DISPLAY_NAME" }) == 3);
  check_executions(state,
    "UPDATE \"App\".\"Accounts\" SET \"Tenant_Id\" = ?, \"User_Id\" = ?, "
    "\"Balance\" = ? WHERE \"Display_Name\" = ?", named_update);
  CHECK(database.remove(rows, { "DISPLAY_NAME" }) == 3);
  check_executions(state,
    "DELETE FROM \"App\".\"Accounts\" WHERE \"Display_Name\" = ?",
    named_remove);
  CHECK_THROWS(database.update(rows.front(), { "User_Id" }), mapping_error);
  CHECK_THROWS(database.remove(rows, { "User_Id" }), mapping_error);
  CHECK(state.executed.empty());
  CHECK(state.catalog.table_reads.size() == 1);
  CHECK(state.catalog.column_reads.size() == 1);
  CHECK((state.column_batches > 0) == columnar);
  CHECK((state.row_batches > 0) == !columnar);
  check_declarations(database.meta<account>());
}

void test_mysql_and_literal_namespace_quoting() {
  recording_fixture fixture;
  auto& state = *fixture.state;
  state.product = "MySQL";
  state.catalog.entries = { accounts_at("Ware.house`x", {}, "Account`Book") };
  state.catalog.entries[0].columns[1].shape.name = "User`Id";
  orm database(fixture.pool);
  database.map<event>("ACCOUNT`BOOK").primary_key("USER`ID", &event::id);
  database.identifier_case(dialect::identifier_case::lower);
  CHECK_THROWS(database.resolve_identifiers("Ware.house`x", "App"),
    mapping_error);
  database.resolve_identifiers("Ware.house`x", {});
  check_resolution(database.meta<event>(),
    { "Ware.house`x", {}, "Account`Book" }, { "User`Id" });
  auto gateway = database.query();
  auto query = gateway.of<event>();
  CHECK(query.build_select() ==
    "SELECT `User``Id` FROM `Ware.house``x`.`Account``Book`");
  CHECK(database.remove(event{ 7 }) == 1);
  check_executions(state,
    "DELETE FROM `Ware.house``x`.`Account``Book` WHERE `User``Id` = ?",
    { params(std::int64_t{ 7 }).values() });
  database.validate();
  CHECK(state.catalog.legacy_reads == 0);

  recording_fixture postgres;
  postgres.state->catalog.entries = {
    accounts_at("Tenant", "A.pp\"x", "Account\"Book") };
  postgres.state->catalog.entries[0].columns[1].shape.name = "User\"Id";
  orm quoted(postgres.pool);
  quoted.map<event>("ACCOUNT\"BOOK").primary_key("USER\"ID", &event::id);
  quoted.resolve_identifiers("Tenant", "A.pp\"x");
  CHECK(quoted.remove(event{ 7 }) == 1);
  check_executions(*postgres.state,
    "DELETE FROM \"A.pp\"\"x\".\"Account\"\"Book\" WHERE \"User\"\"Id\" = ?",
    { params(std::int64_t{ 7 }).values() });
}

}  // namespace

void test_identifier_resolution() {
  test_exact_scope_and_order();
  test_resolution_failures_are_atomic();
  test_ambiguous_missing_and_conflicting_catalogs();
  test_new_registration_and_retained_builders();
  test_resolved_validation_uses_exact_names();
  test_non_ascii_names_are_byte_preserving();
  test_scope_requirements_and_unsupported_backends();
  test_lifecycle_and_pool_reuse();
  test_queries_and_statement_cache();
  test_direct_crud(false);
  test_direct_crud(true);
  test_mysql_and_literal_namespace_quoting();
}
