// The folded batch write: what one statement per chunk looks like for each
// family, how many rows it carries, and what it answers with. Two of the four
// shapes cannot be reached from a live server in this environment, so the text
// itself is what these checks hold still.
#include "check.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/error.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/detail/time.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/error.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/params.hpp>
#include <uniorm/pool.hpp>
#include <uniorm/schema.hpp>
#include <uniorm/types.hpp>

using namespace uniorm;

namespace {

struct account {
  std::int64_t id = 0;
  std::string name;
};

// Two columns make the key, so a folded match has more than one condition.
struct pair_row {
  std::int64_t grp = 0;
  std::int64_t index = 0;
  std::string label;
};

// The shapes an array literal has to spell: no value, a value holding the
// array's own punctuation, and a byte string the server reads in hex.
struct note_row {
  std::int64_t id = 0;
  std::optional<std::string> note;
  std::vector<std::byte> blob;
};

// A day, a clock and a moment of both are the same kind in the entity, and
// only the word a column's type is filed under tells them apart.
struct stamp_row {
  std::int64_t id = 0;
  std::optional<timestamp> moment;
  std::optional<timestamp> day;
  std::optional<timestamp> clock;
};

// Nine columns, so a chunk of them runs into the parameter ceiling well under
// the row count a test can hold in hand.
struct wide_account {
  std::int64_t id = 0;
  std::int64_t col_a = 0;
  std::int64_t col_b = 0;
  std::int64_t col_c = 0;
  std::int64_t col_d = 0;
  std::int64_t col_e = 0;
  std::int64_t col_f = 0;
  std::int64_t col_g = 0;
  std::int64_t col_h = 0;
};

// A second mapping of the same wide table that names only the key and one
// column, which is how a batch update writes a subset of the table's columns.
struct partial_wide {
  std::int64_t id = 0;
  std::int64_t col_a = 0;
};

// One execute's worth of what the core asked the server for: the statement it
// prepared, the values it bound and the ordinal each went to, and -- for the
// array route -- how many sets one execute was handed.
struct fold_execute {
  std::string text;
  std::vector<std::string> binds;
  std::size_t array_sets = 0;
};

struct fold_state {
  std::vector<fold_execute> executes;
  std::string server = "PostgreSQL";
  // What the server says of itself, which is what an array row source waits
  // for, and the words its catalog calls each column's type.
  std::string version;
  std::vector<std::pair<std::string, std::string>> types;
  // What one execute answers for the rows it touched.
  std::size_t report = 1;
  bool columnar_batch = false;
  bool array_rowcount_totals = false;
  bool array_single_send = true;
  bool array_insert = true;
  bool array_update_delete = true;
};

// The catalog read the array row source is spelled from: the configured names
// and nothing beside them, since a fold asks what a column is and no more.
class fold_schema : public schema_meta {
public:
  explicit fold_schema(std::shared_ptr<fold_state> state)
    : state_(std::move(state)) {}

  std::string database_name() override {
    return "folddb";
  }

  std::vector<table_row> tables(std::string_view, std::string_view) override {
    return {};
  }

  std::vector<column_row> table_columns(table_ref const&) override {
    std::vector<column_row> rows;
    for (auto const& [name, spelling] : state_->types) {
      column_row row;
      row.shape.name = name;
      row.type_name = spelling;
      rows.push_back(std::move(row));
    }
    return rows;
  }

  std::vector<std::string> primary_key(table_ref const&) override {
    throw backend::capability_not_supported("this catalog names no keys");
  }

  std::vector<foreign_key_row> foreign_keys(table_ref const&) override {
    throw backend::capability_not_supported("this catalog names no keys");
  }

  std::vector<index_row> indexes(table_ref const&) override {
    throw backend::capability_not_supported("this catalog names no keys");
  }

private:
  std::shared_ptr<fold_state> state_;
};

std::string render(sql_value const& value) {
  return std::visit(
    [](auto const& held) -> std::string {
      using held_type = std::decay_t<decltype(held)>;
      if constexpr (std::is_same_v<held_type, std::monostate>) {
        return "NULL";
      } else if constexpr (std::is_same_v<held_type, std::string>) {
        return held;
      } else if constexpr (std::is_same_v<held_type, std::vector<std::byte>>) {
        return "bytes";
      } else if constexpr (std::is_same_v<held_type, timestamp>) {
        return std::to_string(
          static_cast<long long>(held.time_since_epoch().count()));
      } else {
        return std::to_string(static_cast<long long>(held));
      }
    },
    value);
}

class fold_statement : public backend::statement_iface {
public:
  explicit fold_statement(std::shared_ptr<fold_state> state)
    : state_(std::move(state)) {}

  void prepare(std::string_view sql) override {
    text_ = std::string(sql);
  }

  void bind_parameter(std::size_t index, sql_value const& value) override {
    binds_.push_back(std::to_string(index) + ":" + render(value));
  }

  void bind_batch_params(std::vector<params> const& rows) override {
    array_sets_ = rows.size();
    // The array route hands one set of values for the whole block, so a bound
    // count of one set is what reaches the record.
    if (!rows.empty()) {
      for (std::size_t position = 0; position < rows.front().size();
        ++position) {
        binds_.push_back(std::to_string(position + 1) + ":" +
                         render(rows.front().at(position)));
      }
    }
  }

  backend::batch_writer_iface& prepare_batch() override {
    throw std::logic_error("a backend without the columnar batch was asked"
                           " for one");
  }

  void bind_column(std::size_t, backend::column_buffer const&) override {}

  void execute() override {
    state_->executes.push_back(fold_execute{ text_, binds_, array_sets_ });
    binds_.clear();
    array_sets_ = 0;
    affected_ = state_->report;
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
    affected_ = 0;
  }

private:
  std::shared_ptr<fold_state> state_;
  std::string text_;
  std::vector<std::string> binds_;
  std::size_t array_sets_ = 0;
  std::size_t affected_ = 0;
};

class fold_connection : public backend::connection_iface {
public:
  explicit fold_connection(std::shared_ptr<fold_state> state)
    : state_(std::move(state)), catalog_(state_) {}

  void open(std::string_view) override {}
  void close() override {}
  bool is_open() const noexcept override {
    return true;
  }
  void set_autocommit(bool) override {}
  void commit() override {}
  void rollback() override {}
  backend::capabilities caps() const noexcept override {
    return { /*columnar_batch=*/state_->columnar_batch,
      /*array_rowcount_totals=*/state_->array_rowcount_totals,
      /*array_single_send=*/state_->array_single_send,
      /*array_insert=*/state_->array_insert,
      /*array_update_delete=*/state_->array_update_delete };
  }
  std::string dbms_name() const override {
    return state_->server;
  }
  std::string server_version() const override {
    return state_->version;
  }
  std::unique_ptr<backend::statement_iface> create_statement() override {
    return std::make_unique<fold_statement>(state_);
  }
  schema_meta* schema() noexcept override {
    return &catalog_;
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<fold_state> state_;
  fold_schema catalog_;
};

// The registry keeps a scheme once it has one, so each configuration gets a
// name of its own.
std::shared_ptr<fold_state> register_fold_backend(std::string_view scheme) {
  auto state = std::make_shared<fold_state>();
  backend::registry::instance().register_backend(std::string(scheme),
    [state] { return std::make_unique<fold_connection>(state); });
  return state;
}

pool_options fold_options(std::string_view scheme) {
  pool_options options;
  options.target = std::string(scheme) + "://test";
  options.size = 1;
  options.acquire_timeout = std::chrono::milliseconds{ 200 };
  options.heartbeat_interval = std::chrono::milliseconds{ 0 };
  return options;
}

// A leased connection must outlive the pool it came from, so both stay as the
// caller's own locals.
struct fold_session {
  std::shared_ptr<fold_state> state;
  connection_pool pool;
  orm database;

  explicit fold_session(std::string_view scheme)
    : state(register_fold_backend(scheme)), pool(fold_options(scheme)),
      database(this->pool) {
    database.map<account>("accounts")
      .primary_key("id", &account::id)
      .column("name", &account::name);
    database.map<pair_row>("pairs")
      .primary_key("grp", &pair_row::grp)
      .primary_key("index", &pair_row::index)
      .column("label", &pair_row::label);
    database.map<wide_account>("wide_accounts")
      .primary_key("id", &wide_account::id)
      .column("col_a", &wide_account::col_a)
      .column("col_b", &wide_account::col_b)
      .column("col_c", &wide_account::col_c)
      .column("col_d", &wide_account::col_d)
      .column("col_e", &wide_account::col_e)
      .column("col_f", &wide_account::col_f)
      .column("col_g", &wide_account::col_g)
      .column("col_h", &wide_account::col_h);
    database.map<note_row>("notes")
      .primary_key("id", &note_row::id)
      .column("note", &note_row::note)
      .column("blob", &note_row::blob);
    database.map<stamp_row>("stamps")
      .primary_key("id", &stamp_row::id)
      .column("moment", &stamp_row::moment)
      .column("day", &stamp_row::day)
      .column("clock", &stamp_row::clock);
  }
};

std::vector<account> accounts(std::initializer_list<account> rows) {
  return std::vector<account>(rows);
}

std::vector<wide_account> wide_rows(std::size_t count) {
  std::vector<wide_account> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    rows.push_back(
      wide_account{ static_cast<std::int64_t>(index), 1, 2, 3, 4, 5, 6, 7, 8 });
  }
  return rows;
}

// The tuple count a folded text carries, read off the parameter marks in its
// row source.
std::size_t folded_sets(std::string const& text) {
  std::size_t count = 0;
  for (std::size_t at = text.find("UNION ALL"); at != std::string::npos;
    at = text.find("UNION ALL", at + 1)) {
    ++count;
  }
  return count;
}

}  // namespace

void test_fold_batch() {
  std::vector<account> const two_rows{ { 7, "ada" }, { 8, "lin" } };
  std::vector<account> const one_row{ { 7, "ada" } };
  std::vector<account> const repeated_key{ { 1, "first" }, { 1, "last" },
    { 2, "two" } };
  std::vector<account> const three_rows{ { 7, "ada" }, { 8, "lin" },
    { 9, "grace" } };
  std::vector<std::string> const two_row_binds{ "1:7", "2:ada", "3:8",
    "4:lin" };
  std::vector<std::string> const deduped_binds{ "1:1", "2:last", "3:2",
    "4:two" };

  // The PostgreSQL update: the new values reach the SET bare -- a qualifier
  // there is read as a column of the target, and an alias is not one -- the
  // keys meet the two sides in the WHERE, and the chunk is a query of its own.
  {
    fold_session session{ "foldpg" };
    session.state->server = "PostgreSQL";
    std::size_t const written = session.database.update(two_rows);
    CHECK(written == session.state->report);
    CHECK(session.state->executes.size() == 1);
    fold_execute const& sent = session.state->executes.front();
    std::string const expected =
      "UPDATE \"accounts\" uo_batch_target SET \"name\" = "
      "uo_batch_rows.\"name\" FROM (SELECT \"id\", \"name\" FROM "
      "\"accounts\" WHERE 1 = 0 UNION ALL SELECT ?, ? UNION ALL SELECT "
      "?, ?) uo_batch_rows WHERE uo_batch_target.\"id\" = "
      "uo_batch_rows.\"id\"";
    CHECK(sent.text == expected);
    CHECK(sent.binds == two_row_binds);
    CHECK(folded_sets(sent.text) == 2);

    // One key named twice is written once, with the later row's values: a
    // folded write picks no tuple between two that share a key.
    session.state->executes.clear();
    CHECK(session.database.update(repeated_key) == session.state->report);
    CHECK(session.state->executes.size() == 1);
    CHECK(folded_sets(session.state->executes.front().text) == 2);
    CHECK(session.state->executes.front().binds == deduped_binds);

    // A delete carries keys only, and the row source is the whole of its USING
    // clause.
    session.state->executes.clear();
    CHECK(session.database.remove(one_row) == session.state->report);
    std::string const deleted =
      "DELETE FROM \"accounts\" uo_batch_target USING (SELECT \"id\" FROM "
      "\"accounts\" WHERE 1 = 0 UNION ALL SELECT ?) uo_batch_rows WHERE "
      "uo_batch_target.\"id\" = uo_batch_rows.\"id\"";
    CHECK(session.state->executes.front().text == deleted);

    // A key of two columns gives the match two conditions, and the row source
    // carries the key parts ahead of the written column.
    session.state->executes.clear();
    std::vector<pair_row> const one_pair{ { 9, 1, "x" } };
    CHECK(session.database.update(one_pair) == session.state->report);
    std::string const key_updated =
      "UPDATE \"pairs\" uo_batch_target SET \"label\" = "
      "uo_batch_rows.\"label\" FROM (SELECT \"grp\", \"index\", \"label\" "
      "FROM \"pairs\" WHERE 1 = 0 UNION ALL SELECT ?, ?, ?) uo_batch_rows "
      "WHERE uo_batch_target.\"grp\" = uo_batch_rows.\"grp\" AND "
      "uo_batch_target.\"index\" = uo_batch_rows.\"index\"";
    CHECK(session.state->executes.front().text == key_updated);
    std::vector<std::string> const key_binds{ "1:9", "2:1", "3:x" };
    CHECK(session.state->executes.front().binds == key_binds);

    session.state->executes.clear();
    CHECK(session.database.remove(one_pair) == session.state->report);
    std::string const key_deleted =
      "DELETE FROM \"pairs\" uo_batch_target USING (SELECT \"grp\", "
      "\"index\" FROM \"pairs\" WHERE 1 = 0 UNION ALL SELECT ?, ?) "
      "uo_batch_rows WHERE uo_batch_target.\"grp\" = uo_batch_rows.\"grp\""
      " AND uo_batch_target.\"index\" = uo_batch_rows.\"index\"";
    CHECK(session.state->executes.front().text == key_deleted);

    // The columns a folded write matches on are whatever the caller wrote a
    // predicate with, not only the key. Two rows sharing them would be picked
    // between by the server, so they are one tuple here and the later row's
    // values are the ones it carries.
    session.state->executes.clear();
    std::vector<pair_row> const same_group{ { 4, 1, "first" },
      { 4, 2, "last" } };
    CHECK(
      session.database.update(same_group, { "grp" }) == session.state->report);
    std::string const group_updated =
      "UPDATE \"pairs\" uo_batch_target SET \"index\" = "
      "uo_batch_rows.\"index\", \"label\" = uo_batch_rows.\"label\" FROM "
      "(SELECT \"grp\", \"index\", \"label\" FROM \"pairs\" WHERE 1 = 0 "
      "UNION ALL SELECT ?, ?, ?) uo_batch_rows WHERE "
      "uo_batch_target.\"grp\" = uo_batch_rows.\"grp\"";
    CHECK(session.state->executes.front().text == group_updated);
    std::vector<std::string> const group_binds{ "1:4", "2:2", "3:last" };
    CHECK(session.state->executes.front().binds == group_binds);
  }

  // The MySQL-wire update: the written-to column is qualified, because an
  // unqualified one is ambiguous across the joined tables, and the join takes
  // the place of the FROM clause.
  {
    fold_session session{ "foldmysql" };
    session.state->server = "MySQL";
    CHECK(session.database.update(two_rows) == session.state->report);
    fold_execute const& sent = session.state->executes.front();
    std::string const expected =
      "UPDATE `accounts` uo_batch_target JOIN (SELECT `id`, `name` FROM "
      "`accounts` WHERE 1 = 0 UNION ALL SELECT ?, ? UNION ALL SELECT ?, ?)"
      " uo_batch_rows ON uo_batch_target.`id` = uo_batch_rows.`id` SET "
      "uo_batch_target.`name` = uo_batch_rows.`name`";
    CHECK(sent.text == expected);
    session.state->executes.clear();
    CHECK(session.database.remove(one_row) == session.state->report);
    std::string const deleted =
      "DELETE uo_batch_target FROM `accounts` uo_batch_target JOIN "
      "(SELECT `id` FROM `accounts` WHERE 1 = 0 UNION ALL SELECT ?) "
      "uo_batch_rows ON uo_batch_target.`id` = uo_batch_rows.`id`";
    CHECK(session.state->executes.front().text == deleted);
  }

  // Oracle: a MERGE against a row source whose literal SELECTs each read from
  // dual, identifiers folded upper as the server stores them.
  {
    fold_session session{ "foldoracle" };
    session.state->server = "Oracle";
    CHECK(session.database.update(one_row) == session.state->report);
    std::string const expected =
      "MERGE INTO \"ACCOUNTS\" uo_batch_target USING (SELECT \"ID\", "
      "\"NAME\" FROM \"ACCOUNTS\" WHERE 1 = 0 UNION ALL SELECT ?, ? FROM "
      "dual) uo_batch_rows ON (uo_batch_target.\"ID\" = "
      "uo_batch_rows.\"ID\") WHEN MATCHED THEN UPDATE SET "
      "uo_batch_target.\"NAME\" = uo_batch_rows.\"NAME\"";
    CHECK(session.state->executes.front().text == expected);
    session.state->executes.clear();
    CHECK(session.database.remove(one_row) == session.state->report);
    std::string const deleted =
      "DELETE FROM \"ACCOUNTS\" uo_batch_target WHERE EXISTS (SELECT 1 "
      "FROM (SELECT \"ID\" FROM \"ACCOUNTS\" WHERE 1 = 0 UNION ALL "
      "SELECT ? FROM dual) uo_batch_rows WHERE uo_batch_rows.\"ID\" = "
      "uo_batch_target.\"ID\")";
    CHECK(session.state->executes.front().text == deleted);
  }

  // SQL Server: the UPDATE's own name is the target alias it declares in its
  // FROM clause, and the tables meet there.
  {
    fold_session session{ "foldmssql" };
    session.state->server = "SQL Server";
    CHECK(session.database.update(one_row) == session.state->report);
    std::string const expected =
      "UPDATE uo_batch_target SET uo_batch_target.\"name\" = "
      "uo_batch_rows.\"name\" FROM \"accounts\" uo_batch_target JOIN "
      "(SELECT \"id\", \"name\" FROM \"accounts\" WHERE 1 = 0 UNION ALL "
      "SELECT ?, ?) uo_batch_rows ON uo_batch_target.\"id\" = "
      "uo_batch_rows.\"id\"";
    CHECK(session.state->executes.front().text == expected);
    session.state->executes.clear();
    CHECK(session.database.remove(one_row) == session.state->report);
    std::string const deleted =
      "DELETE uo_batch_target FROM \"accounts\" uo_batch_target JOIN "
      "(SELECT \"id\" FROM \"accounts\" WHERE 1 = 0 UNION ALL SELECT ?) "
      "uo_batch_rows ON uo_batch_target.\"id\" = uo_batch_rows.\"id\"";
    CHECK(session.state->executes.front().text == deleted);
  }

  // The VALUES constructor a versioned SQL Server answers with: one list of
  // parameter groups, the column names on the alias, no seed SELECT and no
  // UNION ALL. The binds are still one group per row.
  {
    fold_session session{ "foldmssqlvalues" };
    session.state->server = "SQL Server";
    session.state->version = "16.0.1000";
    CHECK(session.database.update(two_rows) == session.state->report);
    fold_execute const& sent = session.state->executes.front();
    std::string const expected =
      "UPDATE uo_batch_target SET uo_batch_target.\"name\" = "
      "uo_batch_rows.\"name\" FROM \"accounts\" uo_batch_target JOIN (VALUES "
      "(?, ?), (?, ?)) AS uo_batch_rows(\"id\", \"name\") ON "
      "uo_batch_target.\"id\" = uo_batch_rows.\"id\"";
    CHECK(sent.text == expected);
    CHECK(sent.binds == two_row_binds);
    CHECK(folded_sets(sent.text) == 0);
    session.state->executes.clear();
    CHECK(session.database.remove(one_row) == session.state->report);
    std::string const deleted =
      "DELETE uo_batch_target FROM \"accounts\" uo_batch_target JOIN (VALUES "
      "(?)) AS uo_batch_rows(\"id\") ON uo_batch_target.\"id\" = "
      "uo_batch_rows.\"id\"";
    CHECK(session.state->executes.front().text == deleted);
  }

  // Oracle 23ai: the same constructor, but the alias takes no AS and the
  // identifiers fold upper as the server stores them.
  {
    fold_session session{ "foldoraclevalues" };
    session.state->server = "Oracle";
    session.state->version = "23.7.0.0.0";
    CHECK(session.database.update(one_row) == session.state->report);
    std::string const expected =
      "MERGE INTO \"ACCOUNTS\" uo_batch_target USING (VALUES (?, ?)) "
      "uo_batch_rows(\"ID\", \"NAME\") ON (uo_batch_target.\"ID\" = "
      "uo_batch_rows.\"ID\") WHEN MATCHED THEN UPDATE SET "
      "uo_batch_target.\"NAME\" = uo_batch_rows.\"NAME\"";
    CHECK(session.state->executes.front().text == expected);
    session.state->executes.clear();
    CHECK(session.database.remove(one_row) == session.state->report);
    std::string const deleted =
      "DELETE FROM \"ACCOUNTS\" uo_batch_target WHERE EXISTS (SELECT 1 FROM "
      "(VALUES (?)) uo_batch_rows(\"ID\") WHERE uo_batch_rows.\"ID\" = "
      "uo_batch_target.\"ID\")";
    CHECK(session.state->executes.front().text == deleted);
  }

  // A VALUES fold still cuts at the parameter ceiling: nine columns a row at
  // Oracle's 3000 is 333 rows a statement, and the tail is its own execute. The
  // row count is read off the binds, since a VALUES text carries no UNION ALL.
  {
    fold_session session{ "foldvaluesceiling" };
    session.state->server = "Oracle";
    session.state->version = "23.7.0.0.0";
    session.database.paramset_size(5000);
    std::vector<wide_account> const many = wide_rows(500);
    CHECK(session.database.update(many) == 2);
    CHECK(session.state->executes.size() == 2);
    for (fold_execute const& sent : session.state->executes) {
      CHECK(sent.text.find("VALUES") != std::string::npos);
      CHECK(folded_sets(sent.text) == 0);
    }
    CHECK(session.state->executes[0].binds.size() == 333 * 9);
    CHECK(session.state->executes[1].binds.size() == 167 * 9);
  }

  // A column every row leaves empty still takes one bind a row in the VALUES
  // list: the row source spells the column, it does not drop it. What the
  // server infers for an all-null column is its own answer, not this text's.
  {
    fold_session session{ "foldvaluesnulls" };
    session.state->server = "Oracle";
    session.state->version = "23.7.0.0.0";
    std::vector<note_row> const nulls{ { 7, std::nullopt, {} },
      { 8, std::nullopt, {} } };
    CHECK(session.database.update(nulls) == session.state->report);
    fold_execute const& sent = session.state->executes.front();
    CHECK(sent.text.find("(VALUES (?, ?, ?), (?, ?, ?))") != std::string::npos);
    CHECK(folded_sets(sent.text) == 0);
    std::vector<std::string> const null_binds{ "1:7", "2:NULL", "3:bytes",
      "4:8", "5:NULL", "6:bytes" };
    CHECK(sent.binds == null_binds);
  }

  // A batch update writes the columns its entity names, no more: a second
  // mapping of the wide table that carries only the key and col_a sets col_a
  // alone, leaving the table's other columns out of the SET and the row source.
  {
    fold_session session{ "foldpartial" };
    session.database.map<partial_wide>("wide_accounts")
      .primary_key("id", &partial_wide::id)
      .column("col_a", &partial_wide::col_a);
    session.state->report = 2;
    std::vector<partial_wide> const rows{ { 1, 10 }, { 2, 20 } };
    CHECK(session.database.update(rows) == 2);
    CHECK(session.state->executes.size() == 1);
    fold_execute const& sent = session.state->executes.front();
    std::string const expected =
      "UPDATE \"wide_accounts\" uo_batch_target SET \"col_a\" = "
      "uo_batch_rows.\"col_a\" FROM (SELECT \"id\", \"col_a\" FROM "
      "\"wide_accounts\" WHERE 1 = 0 UNION ALL SELECT ?, ? UNION ALL SELECT "
      "?, ?) uo_batch_rows WHERE uo_batch_target.\"id\" = "
      "uo_batch_rows.\"id\"";
    CHECK(sent.text == expected);
    CHECK(sent.text.find("col_b") == std::string::npos);
    CHECK(folded_sets(sent.text) == 2);
    std::vector<std::string> const partial_binds{ "1:1", "2:10", "3:2",
      "4:20" };
    CHECK(sent.binds == partial_binds);
  }

  // The ceiling, not the asked chunk, decides how a sweep splits: nine columns
  // a row at the 4000-parameter ceiling is 444 rows a statement, and the tail
  // is its own text because a chunk of 56 tuples is a different statement.
  {
    fold_session session{ "foldceiling" };
    session.database.paramset_size(5000);
    std::vector<wide_account> const many = wide_rows(500);
    // Two executes, each answering with the one count the fake gives.
    CHECK(session.database.update(many) == 2);
    CHECK(session.state->executes.size() == 2);
    CHECK(folded_sets(session.state->executes[0].text) == 444);
    CHECK(folded_sets(session.state->executes[1].text) == 56);

    // An asked chunk smaller than the ceiling still decides the cut.
    session.state->executes.clear();
    session.database.paramset_size(2);
    std::vector<wide_account> const five = wide_rows(5);
    CHECK(session.database.remove(five) == 3);
    CHECK(session.state->executes.size() == 3);
    CHECK(folded_sets(session.state->executes[0].text) == 2);
    CHECK(folded_sets(session.state->executes[2].text) == 1);
  }

  // What the fold leaves alone: an array that both counts the whole block and
  // sends it once is the cheaper route, so a known family does not displace it.
  {
    fold_session session{ "foldkept" };
    session.state->server = "PostgreSQL";
    session.state->array_rowcount_totals = true;
    CHECK(session.database.update(two_rows) == session.state->report);
    CHECK(session.state->executes.size() == 1);
    CHECK(session.state->executes.front().array_sets == 2);
    CHECK(session.state->executes.front().text.find("UNION ALL") ==
          std::string::npos);
  }

  // An array that counts honestly but costs a round trip per set is the case
  // the fold exists for: one statement per chunk instead.
  {
    fold_session session{ "foldlibpq" };
    session.state->server = "PostgreSQL";
    session.state->array_rowcount_totals = true;
    session.state->array_single_send = false;
    CHECK(session.database.update(three_rows) == session.state->report);
    CHECK(session.state->executes.size() == 1);
    CHECK(folded_sets(session.state->executes.front().text) == 3);
    CHECK(session.state->executes.front().array_sets == 0);
  }

  // The MySQL-wire ODBC connectors (MariaDB Connector/ODBC and the MySQL one):
  // the array counts the whole block, but an array UPDATE/DELETE renders as
  // multi-statement text the driver cannot execute, so the backend reports
  // array_update_delete=false. Even with the columnar batch on offer, automatic
  // must fold the write rather than take the array path that would throw.
  {
    fold_session session{ "foldmysqlwire" };
    session.state->server = "MySQL";
    session.state->columnar_batch = true;
    session.state->array_rowcount_totals = true;
    session.state->array_update_delete = false;
    CHECK(session.database.update(two_rows) == session.state->report);
    CHECK(session.state->executes.size() == 1);
    CHECK(folded_sets(session.state->executes.front().text) == 2);
    CHECK(session.state->executes.front().array_sets == 0);
    CHECK(
      session.state->executes.front().text.find("JOIN") != std::string::npos);

    session.state->executes.clear();
    CHECK(session.database.remove(one_row) == session.state->report);
    CHECK(folded_sets(session.state->executes.front().text) == 1);
    CHECK(session.state->executes.front().array_sets == 0);
  }

  // The same driver asked for the columnar path explicitly: the array UPDATE
  // is unexecutable, so the request is refused rather than sent to a driver
  // that would drop it or throw.
  {
    fold_session session{ "foldmysqlwirecolumnar" };
    session.state->server = "MySQL";
    session.state->columnar_batch = true;
    session.state->array_rowcount_totals = true;
    session.state->array_update_delete = false;
    session.database.batch_write_policy(orm::batch_policy::columnar);
    CHECK_THROWS(session.database.update(two_rows), uniorm_error);
  }

  // A driver that cannot execute the array and a server with no folded shape:
  // automatic falls back to one parameter set per execute, never an array.
  {
    fold_session session{ "foldnoarray" };
    session.state->server = "batch";
    session.state->columnar_batch = true;
    session.state->array_rowcount_totals = true;
    session.state->array_update_delete = false;
    // One parameter set per row tallies each execute on its own.
    CHECK(session.database.update(two_rows) == 2 * session.state->report);
    CHECK(session.state->executes.size() == 2);
    for (auto const& sent : session.state->executes) {
      CHECK(sent.array_sets == 1);
    }
  }

  // The same family once the server names a version that grew the array row
  // source and the catalog names each column: the chunk arrives as one array
  // per column, so two parameters carry every row of it and the text does not
  // grow with the batch.
  {
    fold_session session{ "foldarrays" };
    session.state->server = "PostgreSQL 18.6";
    session.state->version = "18.6";
    session.state->types = { { "id", "int8" }, { "name", "text" } };
    CHECK(session.database.update(two_rows) == session.state->report);
    std::string const updated =
      "UPDATE \"accounts\" uo_batch_target SET \"name\" = "
      "uo_batch_rows.\"name\" FROM unnest(?::int8[], ?::text[]) AS "
      "uo_batch_rows(\"id\", \"name\") WHERE uo_batch_target.\"id\" = "
      "uo_batch_rows.\"id\"";
    fold_execute const& sent = session.state->executes.front();
    CHECK(sent.text == updated);
    // Every element is quoted but the one the entity leaves empty, which the
    // bare word alone spells as no value.
    std::vector<std::string> const spelled{ R"(1:{"7","8"})",
      R"(2:{"ada","lin"})" };
    CHECK(sent.binds == spelled);

    // A chunk of one row is the same statement: nothing in the text counts
    // the rows, so a sweep of any depth reads one parse.
    session.state->executes.clear();
    CHECK(session.database.update(one_row) == session.state->report);
    CHECK(session.state->executes.front().text == updated);

    // The key columns of a delete are the arrays it carries, and a repeated
    // key is still written once, with the later row's value.
    session.state->executes.clear();
    CHECK(session.database.remove(two_rows) == session.state->report);
    std::string const deleted =
      "DELETE FROM \"accounts\" uo_batch_target USING unnest(?::int8[]) "
      "AS uo_batch_rows(\"id\") WHERE uo_batch_target.\"id\" = "
      "uo_batch_rows.\"id\"";
    CHECK(session.state->executes.front().text == deleted);
    session.state->executes.clear();
    CHECK(session.database.update(repeated_key) == session.state->report);
    std::vector<std::string> const deduped{ R"(1:{"1","2"})",
      R"(2:{"last","two"})" };
    CHECK(session.state->executes.front().binds == deduped);

    // A two-column key hands the row source two arrays, and the written
    // column follows them.
    fold_session keys{ "foldarraykeys" };
    keys.state->version =
      "9.4";  // the release that grew the multi-argument call
    keys.state->types = { { "grp", "int8" }, { "index", "int8" },
      { "label", "varchar" } };
    std::vector<pair_row> const one_pair{ { 9, 1, "x" } };
    CHECK(keys.database.update(one_pair) == keys.state->report);
    std::string const key_updated =
      "UPDATE \"pairs\" uo_batch_target SET \"label\" = "
      "uo_batch_rows.\"label\" FROM unnest(?::int8[], ?::int8[], "
      "?::varchar[]) AS uo_batch_rows(\"grp\", \"index\", \"label\") "
      "WHERE uo_batch_target.\"grp\" = uo_batch_rows.\"grp\" AND "
      "uo_batch_target.\"index\" = uo_batch_rows.\"index\"";
    CHECK(keys.state->executes.front().text == key_updated);
  }

  // What keeps a sweep off the array source: a server that did not say, one
  // that said an older release, a column the catalog does not name, and a type
  // whose own name is already an array. Each of them answers with the seeded
  // tuples, which need none of those things to be known.
  {
    std::vector<account> const rows{ { 7, "ada" }, { 8, "lin" } };
    fold_session unsaid{ "foldunsaid" };
    unsaid.state->types = { { "id", "int8" }, { "name", "text" } };
    CHECK(unsaid.database.update(rows) == unsaid.state->report);
    CHECK(
      unsaid.state->executes.front().text.find("unnest") == std::string::npos);

    fold_session older{ "foldolder" };
    older.state->version = "9.3.25";
    older.state->types = { { "id", "int8" }, { "name", "text" } };
    CHECK(older.database.update(rows) == older.state->report);
    CHECK(
      older.state->executes.front().text.find("unnest") == std::string::npos);

    fold_session unnamed{ "foldunnamed" };
    unnamed.state->version = "18.6";
    unnamed.state->types = { { "id", "int8" } };
    CHECK(unnamed.database.update(rows) == unnamed.state->report);
    CHECK(
      unnamed.state->executes.front().text.find("unnest") == std::string::npos);

    fold_session nested{ "foldnested" };
    nested.state->version = "18.6";
    nested.state->types = { { "id", "int8" }, { "name", "_text" } };
    CHECK(nested.database.update(rows) == nested.state->report);
    CHECK(
      nested.state->executes.front().text.find("unnest") == std::string::npos);

    // A name carrying punctuation the cast list has no room for is refused
    // too: what a catalog calls a type is a type's name, never SQL of its own.
    fold_session spelled{ "foldspelled" };
    spelled.state->version = "18.6";
    spelled.state->types = { { "id", "int8" }, { "name", "text) FROM x -- " } };
    CHECK(spelled.database.update(rows) == spelled.state->report);
    CHECK(
      spelled.state->executes.front().text.find("unnest") == std::string::npos);
  }

  // The array source is what frees a sweep from the parameter ceiling: nine
  // columns a row, where the seeded tuples stopped at 444 rows a statement,
  // arrive in the one chunk the caller asked for.
  {
    fold_session session{ "foldarraywide" };
    session.state->version = "18.6";
    session.state->types = { { "id", "int8" }, { "col_a", "int8" },
      { "col_b", "int8" }, { "col_c", "int8" }, { "col_d", "int8" },
      { "col_e", "int8" }, { "col_f", "int8" }, { "col_g", "int8" },
      { "col_h", "int8" } };
    session.database.paramset_size(5000);
    std::vector<wide_account> const many = wide_rows(500);
    CHECK(session.database.update(many) == session.state->report);
    CHECK(session.state->executes.size() == 1);
    CHECK(session.state->executes.front().binds.size() == 9);

    // One array per column, each holding one element per row of the chunk.
    session.state->executes.clear();
    std::vector<wide_account> const three = wide_rows(3);
    CHECK(session.database.update(three) == session.state->report);
    CHECK(
      session.state->executes.front().binds.front() == R"(1:{"0","1","2"})");
    CHECK(session.state->executes.front().binds.back() == R"(9:{"8","8","8"})");

    // An asked chunk still decides the cut.
    session.state->executes.clear();
    session.database.paramset_size(2);
    std::vector<wide_account> const five = wide_rows(5);
    CHECK(session.database.remove(five) == 3);
    CHECK(session.state->executes.size() == 3);
    CHECK(session.state->executes[2].binds.front() == R"(1:{"4"})");
  }

  // The words a value arrives in. Quoting is what keeps the array's own
  // punctuation, and the four letters of a word like NULL, inside the value:
  // only a value the entity leaves empty is written bare, because the bare
  // word is the one spelling the server reads as no value at all.
  {
    fold_session session{ "foldarrayvalues" };
    session.state->version = "18.6";
    session.state->types = { { "id", "int8" }, { "note", "text" },
      { "blob", "bytea" } };
    std::vector<note_row> const rows{
      note_row{ 1, std::nullopt, std::vector<std::byte>{ std::byte{ 0 } } },
      note_row{ 2, std::string{ "a,b\"c\\d{e}" },
        std::vector<std::byte>{ std::byte{ 0x0a }, std::byte{ 0xff } } },
      note_row{ 3, std::string{ "NULL" }, {} },
    };
    CHECK(session.database.update(rows) == session.state->report);
    fold_execute const& sent = session.state->executes.front();
    CHECK(sent.binds.size() == 3);
    CHECK(sent.binds[1] == R"(2:{NULL,"a,b\"c\\d{e}","NULL"})");
    CHECK(sent.binds[2] == R"(3:{"\\x00","\\x0aff","\\x"})");

    // A NUL would cut the whole literal short, which is a different failure
    // from the truncated value a bound parameter would give, so it is refused.
    std::vector<note_row> const cut{ note_row{
      4, std::string{ "a\0b", 3 }, {} } };
    std::string refusal;
    try {
      session.database.update(cut);
    } catch (uniorm_error const& error) {
      refusal = error.what();
    }
    CHECK(refusal.find("NUL") != std::string::npos);
  }

  // The one value whose spelling is not in the value itself. A moment of the
  // day-and-clock kind keeps both halves, a day loses the clock it carries and
  // a clock loses its day: a server reads a date with a clock on it as the
  // wrong shape, and the fold would learn that only from the row that failed.
  {
    fold_session session{ "foldarraytimes" };
    session.state->version = "18.6";
    session.state->types = { { "id", "int8" }, { "moment", "timestamp" },
      { "day", "date" }, { "clock", "time" } };
    std::vector<stamp_row> const rows{ stamp_row{ 1,
      detail::make_timestamp(2024, 12, 31, 23, 59, 59, 123456000UL),
      detail::make_timestamp(2024, 7, 1, 0, 0, 0, 0),
      detail::make_timestamp(1970, 1, 1, 12, 34, 56, 0) } };
    CHECK(session.database.update(rows) == session.state->report);
    fold_execute const& sent = session.state->executes.front();
    CHECK(sent.binds[1] == R"(2:{"2024-12-31 23:59:59.123456"})");
    CHECK(sent.binds[2] == R"(3:{"2024-07-01"})");
    CHECK(sent.binds[3] == R"(4:{"12:34:56"})");
  }

  // The gate the shape waits on, read off the words a server gives for itself.
  {
    dialect unsaid = dialect::detect("PostgreSQL");
    CHECK(unsaid.server_version == 0);
    CHECK(unsaid.batch_rows == dialect::batch_source::parameter_tuples);
    // A banner carries the version among its other words, and a release with no
    // second number still reads beside one with two.
    unsaid.accept_server_version("18.6 (Debian 18.6-1.pgdg13+2)");
    CHECK(unsaid.server_version == 1806);
    CHECK(unsaid.batch_rows == dialect::batch_source::typed_arrays);
    // The release that grew the multi-argument call, and the one before it.
    dialect floor = dialect::detect("PostgreSQL");
    floor.accept_server_version("9.4.20");
    CHECK(floor.server_version == 904);
    CHECK(floor.batch_rows == dialect::batch_source::typed_arrays);
    floor.accept_server_version("9.3");
    CHECK(floor.batch_rows == dialect::batch_source::parameter_tuples);
    // Words that carry no version at all leave the shape the family has always
    // had; a family without the array source is not read for one.
    floor.accept_server_version("PostgreSQL");
    CHECK(floor.server_version == 0);
    CHECK(floor.batch_rows == dialect::batch_source::parameter_tuples);
    dialect other = dialect::detect("MySQL 8.0.36");
    other.accept_server_version("8.0.36");
    CHECK(other.batch_rows == dialect::batch_source::parameter_tuples);
    // Oracle grew the VALUES constructor in 23ai; the release before it keeps
    // the seeded tuples.
    dialect oracle = dialect::detect("Oracle");
    oracle.accept_server_version("23.7.0.0.0");
    CHECK(oracle.batch_rows == dialect::batch_source::value_constructors);
    oracle.accept_server_version("19.0.0.0.0");
    CHECK(oracle.batch_rows == dialect::batch_source::parameter_tuples);
    // SQL Server has had it since 2008 (10.0); 2005 (9.0) is before it.
    dialect mssql = dialect::detect("SQL Server");
    mssql.accept_server_version("10.0.1600");
    CHECK(mssql.batch_rows == dialect::batch_source::value_constructors);
    mssql.accept_server_version("9.0.5000");
    CHECK(mssql.batch_rows == dialect::batch_source::parameter_tuples);
    // A banner with no version leaves the family on the tuples it can build
    // from nothing.
    dialect unsaid_mssql = dialect::detect("SQL Server");
    CHECK(unsaid_mssql.batch_rows == dialect::batch_source::parameter_tuples);
  }

  // The routing policy. A backend whose array both counts the whole block and
  // carries it at once is one automatic leaves unfolded; folded forces the
  // fold past that, and columnar refuses the fold even where automatic takes
  // it.
  {
    fold_session session{ "foldpolicy" };
    session.state->server = "PostgreSQL";
    session.state->array_rowcount_totals = true;
    session.state->array_single_send = true;
    CHECK(
      session.database.batch_write_policy() == orm::batch_policy::automatic);

    CHECK(session.database.update(two_rows) == session.state->report);
    CHECK(folded_sets(session.state->executes.front().text) == 0);

    session.state->executes.clear();
    session.database.batch_write_policy(orm::batch_policy::folded);
    CHECK(session.database.batch_write_policy() == orm::batch_policy::folded);
    CHECK(session.database.update(two_rows) == session.state->report);
    CHECK(session.state->executes.size() == 1);
    CHECK(folded_sets(session.state->executes.front().text) == 2);

    session.state->array_rowcount_totals = false;
    session.state->executes.clear();
    session.database.batch_write_policy(orm::batch_policy::columnar);
    CHECK(session.database.update(two_rows) == session.state->report);
    CHECK(folded_sets(session.state->executes.front().text) == 0);
  }

  // A server with no folded shape cannot be forced into one.
  {
    fold_session session{ "foldpolicynone" };
    session.state->server = "SomeServer";
    session.database.batch_write_policy(orm::batch_policy::folded);
    CHECK_THROWS(session.database.update(two_rows), uniorm_error);
    CHECK_THROWS(session.database.remove(two_rows), uniorm_error);
  }
}
