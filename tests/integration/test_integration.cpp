// Integration tests against a live database, driven through one of the plugin
// backends. UNIORM_IT_BACKEND names the scheme -- "odbc" (no setting needed)
// or "oracle" -- and UNIORM_IT_DSN / UNIORM_IT_USER / UNIORM_IT_PWD carry the
// target in that scheme's own grammar: a registered DSN for odbc, a
// host[:port]/service for oracle. Returns 77 (ctest SKIP) when any of them is
// unset or the database is unreachable. ASCII data only for now.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../unit/check.hpp"
#include "identifier.hpp"

#include <uniorm/async.hpp>
#include <uniorm/backend/error.hpp>
#include <uniorm/connection.hpp>
#include <uniorm/connection_spec.hpp>
#include <uniorm/converter.hpp>
#include <uniorm/decimal.hpp>
#include <uniorm/detail/time.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/pool.hpp>
#include <uniorm/builder/builder.hpp>
#include <uniorm/transaction.hpp>
#include <uniorm/value.hpp>

using namespace uniorm;

namespace uniorm {

struct User {
  std::int64_t id = 0;
  std::string name;
  std::optional<std::int32_t> age;
  double balance = 0.0;
  std::optional<std::string> note;
  std::optional<timestamp> created;
};

struct Pair {
  std::int64_t grp = 0;
  std::int64_t idx = 0;
  std::string label;
};

// Exact numerics as members rather than as the text a DECIMAL defaults to.
struct Money {
  std::int64_t id = 0;
  decimal_t amount;
  std::optional<decimal_t> wide;
  decimal_t whole;
};

// A domain type the database stores as text rather than as its ordinal.
enum class grade { bronze, silver, gold };

struct Order {
  std::int64_t id = 0;
  grade state = grade::bronze;
  std::optional<grade> note;
};

template <>
struct converter<grade> {
  using db_type = std::string;

  static void to_db(grade const& g, std::string& out) {
    if (g == grade::silver)
      out = "silver";
    else if (g == grade::gold)
      out = "gold";
    else
      out = "bronze";
  }

  static grade from_db(std::string const& v) {
    if (v == "silver")
      return grade::silver;
    if (v == "gold")
      return grade::gold;
    return grade::bronze;
  }
};

// Mapped by a name spelled the way this server does not store it, so the
// spelling policy has something to reconcile.
struct CaseUser {
  std::int64_t id = 0;
  std::string name;
};

}  // namespace uniorm

namespace {

char const* k_table = "uniorm_it_user";
char const* k_pair_table = "uniorm_it_pair";
char const* k_order_table = "uniorm_it_order";
char const* k_dec_table = "uniorm_it_decimal";
char const* k_money_table = "uniorm_it_money";
char const* k_num_table = "UNIORM_IT_NUMBER";
// Unquoted DDL, so each server folds it the way it folds any name: MySQL and
// MariaDB keep this spelling, PostgreSQL answers with all lower.
char const* k_case_table = "UNIORM_IT_CASE_USER";
std::string const long_note(1000, 'x');

// The face this run drives. One suite serves both backends, so everything the
// two do differently is either asked of the server or recorded from the scheme
// that opened it -- never assumed from the leg that was written first.
struct target_face {
  std::string scheme;          // the plugin this run loads
  std::string connect_string;  // what a connection is handed
  std::string dsn;             // the target, in this scheme's grammar
  std::string user;
  std::string password;
  // A 64-bit integral column, as this server's DDL spells it.
  std::string integer_type;
  std::string wide_decimal;    // the widest scale this server's DECIMAL takes
  // Whether a decimal's text arrives carrying the column's declared scale.
  bool pads_decimal_text = true;
  // Whether this backend answers catalog reads at all; probed, not declared,
  // so a backend that gains introspection picks these stages up by itself.
  bool answers_catalog = false;
  // Whether this backend holds its parses on the session under a name of its
  // own, which is the one thing a server can be asked to give back.
  bool holds_its_parses_by_name = false;
  dialect server_dialect;      // detected from the banner this run read
};

target_face the_target;

// Set by a stage that gives up on purpose, so the runner can name it skipped
// rather than counting a stage that checked nothing as one that passed.
char const* skip_reason = nullptr;

// A dynamic read asks for a column by the label the server reports, which on a
// server that folds unquoted names is not the spelling the query wrote.
std::string label(std::string_view name) {
  return the_target.server_dialect.fold_identifier(name);
}

// The two spellings a decimal column's text arrives in. A driver that formats
// the value pads it to the column's scale; a server answering with its own
// conversion gives the digits the value has. Both name the same number, and
// this says which one this run was handed.
char const* decimal_text(char const* scaled, char const* minimal) {
  return the_target.pads_decimal_text ? scaled : minimal;
}

// Whatever this driver calls an error's code. SQLSTATE is five characters;
// this server's number is an ORA- code. Either way what() puts it in brackets.
bool state_is_well_formed(std::string_view state) {
  if (the_target.scheme == "oracle") {
    return state.rfind("ORA-", 0) == 0 && state.size() == 9;
  }
  return state.size() == 5;
}

// A few stages ask the server what it holds, which a backend is free not to
// answer. Saying so keeps a skip from reading as a pass.
bool catalog_available() {
  if (the_target.answers_catalog) {
    return true;
  }
  skip_reason = "this backend offers no catalog reads";
  return false;
}

void prepare_schema(orm& db) {
  std::string const id_type = the_target.integer_type;
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_table);
  db.execute_update(std::string("CREATE TABLE ") + k_table +
                      " (id " + id_type + " NOT NULL PRIMARY KEY,"
                      " name VARCHAR(64) NOT NULL,"
                      " age INT NULL,"
                      " balance DOUBLE PRECISION NOT NULL,"
                      " note VARCHAR(2000) NULL,"
                      " created TIMESTAMP NULL)");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_pair_table);
  db.execute_update(std::string("CREATE TABLE ") + k_pair_table +
                      " (grp " + id_type + " NOT NULL,"
                      " idx " + id_type + " NOT NULL,"
                      " label VARCHAR(64) NOT NULL,"
                      " PRIMARY KEY (grp, idx))");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_order_table);
  db.execute_update(std::string("CREATE TABLE ") + k_order_table +
                      " (id " + id_type + " NOT NULL PRIMARY KEY,"
                      " state VARCHAR(16) NOT NULL,"
                      " note VARCHAR(16) NULL)");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_dec_table);
  db.execute_update(std::string("CREATE TABLE ") + k_dec_table +
                      " (id " + id_type + " NOT NULL PRIMARY KEY,"
                      " amount DECIMAL(20,4) NULL,"
                      " whole DECIMAL(20,0) NULL,"
                      " big DECIMAL(38,0) NULL)");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_money_table);
  db.execute_update(std::string("CREATE TABLE ") + k_money_table +
                      " (id " + id_type + " NOT NULL PRIMARY KEY,"
                      " amount DECIMAL(20,4) NOT NULL,"
                      " wide " + the_target.wide_decimal + " NULL,"
                      " whole DECIMAL(20,0) NOT NULL)");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_case_table);
  db.execute_update(std::string("CREATE TABLE ") + k_case_table +
                      " (ID " + id_type + " NOT NULL PRIMARY KEY,"
                      " NAME VARCHAR(64) NOT NULL)");
  db.execute_update(std::string("INSERT INTO ") + k_case_table +
                      " (ID, NAME) VALUES (1, 'raised')");
}

void seed_rows(orm& db) {
  timestamp ts = detail::make_timestamp(2024, 1, 2, 3, 4, 5, 0);
  std::size_t n1 = db.execute_update(
    "INSERT INTO uniorm_it_user (id, name, age, balance, note, created)"
    " VALUES (?, ?, ?, ?, ?, ?)",
    params{ std::int64_t{ 1 }, std::string("alice"), std::int32_t{ 30 }, 12.5,
      nullptr, ts });
  CHECK(n1 == 1);
  std::size_t n2 = db.execute_update(
    "INSERT INTO uniorm_it_user (id, name, age, balance, note, created)"
    " VALUES (?, ?, ?, ?, ?, ?)",
    params{ std::int64_t{ 2 }, std::string("bob"), nullptr, 0.0, long_note,
      nullptr });
  CHECK(n2 == 1);
  std::size_t n3 = db.execute_update(
    "INSERT INTO uniorm_it_user (id, name, age, balance, note, created)"
    " VALUES (?, ?, ?, ?, ?, ?)",
    params{ std::int64_t{ 3 }, std::string("carol"), std::int32_t{ 25 }, -4.25,
      std::string("short"), nullptr });
  CHECK(n3 == 1);
}

void test_dynamic_rows(orm& db) {
  result_set rs =
    db.execute("SELECT id, name, age, note FROM uniorm_it_user WHERE id = ?",
      params{ std::int64_t{ 2 } });
  CHECK(rs.column_count() == 4);
  CHECK(rs.next());
  row r = rs.current();
  CHECK(r.get<std::int64_t>(label("id")) == 2);
  CHECK(r.get<std::int32_t>(0) == 2);  // by index, widened read
  CHECK(r.get<std::string>(label("name")) == "bob");
  CHECK(r.is_null(label("age")));
  CHECK(r.get<std::string>(label("note")).size() == long_note.size());
  CHECK(!rs.next());
}

void test_decimal_dynamic(orm& db) {
  db.execute_update(std::string("DELETE FROM ") + k_dec_table);
  db.execute_update(std::string("INSERT INTO ") + k_dec_table +
                    " (id, amount, whole, big) VALUES"
                    " (1, '12345678901234.5678', '42',"
                    " '123456789012345678901234567890')");
  db.execute_update(std::string("INSERT INTO ") + k_dec_table +
                    " (id, amount, whole, big) VALUES (2, '0.1', '0', '0')");
  db.execute_update(std::string("INSERT INTO ") + k_dec_table +
                    " (id, amount, whole, big) VALUES (3, NULL, NULL, NULL)");

  result_set rs = db.execute(
    std::string("SELECT id, amount, whole, big FROM ") + k_dec_table +
    " ORDER BY id");
  CHECK(rs.column(1).type == sql_type::decimal);
  CHECK(rs.column(1).scale == 4);
  CHECK(rs.column(1).display_size >= 20);

  CHECK(rs.next());
  row r1 = rs.current();
  CHECK(r1.get<std::string>(label("amount")) == "12345678901234.5678");
  double amount = r1.get<double>(label("amount"));
  CHECK(amount > 12345678901234.56 && amount < 12345678901234.57);
  CHECK_THROWS(r1.get<std::int64_t>(label("amount")), type_mismatch);
  CHECK(r1.get<std::string>(label("whole")) == "42");
  CHECK(r1.get<std::int64_t>(label("whole")) == 42);
  CHECK(r1.get<std::string>(label("big")) == "123456789012345678901234567890");
  CHECK_THROWS(r1.get<std::int64_t>(label("big")), type_mismatch);

  CHECK(rs.next());
  row r2 = rs.current();
  CHECK(r2.get<std::string>(label("amount")) ==
        decimal_text("0.1000", ".1"));  // the column's scale, or not

  CHECK(rs.next());
  row r3 = rs.current();
  CHECK(r3.is_null(label("amount")));
  CHECK_THROWS(r3.get<std::string>(label("amount")), type_mismatch);
  CHECK(!rs.next());
}

// A bare NUMBER carries no declared width to size a text buffer from, and its
// shortest spelling runs past the 64 characters a fixed one would stop at. Only
// Oracle spells a column this way, so the ODBC legs give the stage up.
void test_number_text_ceiling(orm& db) {
  if (the_target.scheme != "oracle") {
    skip_reason = "a bare NUMBER column is Oracle's own spelling";
    return;
  }
  try {
    db.execute_update(std::string("DROP TABLE ") + k_num_table);
  } catch (...) {
  }
  db.execute_update(std::string("CREATE TABLE ") + k_num_table +
                    " (id NUMBER, spell NUMBER)");
  db.execute_update(std::string("INSERT INTO ") + k_num_table +
                    " (id, spell) VALUES (1, 1.0E-80)");
  result_set rs = db.execute(
    std::string("SELECT id, spell FROM ") + k_num_table + " ORDER BY id");
  CHECK(rs.next());
  row r = rs.current();
  std::string const text = r.get<std::string>(label("spell"));
  CHECK(text.size() > 64);
  CHECK(text.front() == '.');
  CHECK(text.back() == '1');
  CHECK(r.get<double>(label("spell")) > 0.0);
  db.execute_update(std::string("DROP TABLE ") + k_num_table);
}

void test_decimal_mapped(orm& db) {
  struct money_row {
    std::int64_t id;
    decimal_t amount;
    std::optional<decimal_t> wide;
  };

  db.execute_update(std::string("DELETE FROM ") + k_money_table);
  std::vector<Money> batch{
    Money{ 1, decimal_t::from_literal("12345678901234.5678"),
      decimal_t::from_literal("0.000000000000000000000000000001"),
      decimal_t::from_literal("42") },
    Money{ 2, decimal_t::from_literal("-0.05"), std::nullopt,
      decimal_t::from_literal("0") },
  };
  CHECK(db.insert(batch) == 2);

  auto back = db.query<Money>().order_by(&Money::id).all();
  CHECK(back.size() == 2);
  CHECK(back[0].amount.to_literal() == "12345678901234.5678");
  CHECK(back[0].whole.to_int64() == 42);
  // The wide column: no integer type holds this, and no double survives it.
  CHECK(back[0].wide.has_value() &&
        back[0].wide->to_literal() == "0.000000000000000000000000000001");
  CHECK_THROWS(back[0].wide->to_int64(), type_mismatch);
  CHECK(back[1].amount.to_literal() == decimal_text("-0.0500", "-0.05"));
  CHECK(back[1].amount == decimal_t::from_literal("-0.05"));
  CHECK(!back[1].wide.has_value());

  auto rows = db.query<money_row>(
    "SELECT id, amount, wide FROM uniorm_it_money ORDER BY id");
  CHECK(rows.size() == 2);
  CHECK(rows[0].amount == back[0].amount);
  CHECK(rows[1].wide == std::nullopt);

  result_set rs = db.execute(
    "SELECT id, amount FROM uniorm_it_money WHERE id = ?",
    params{ std::int64_t{ 2 } });
  CHECK(rs.next());
  row r = rs.current();
  CHECK(r.get<decimal_t>(label("amount")) == decimal_t::from_literal("-0.05"));
  CHECK(r.get<std::string>(label("amount")) == decimal_text("-0.0500", "-.05"));

  // A decimal_t query value reaches the driver as that same literal.
  CHECK(db.query<Money>()
          .where(gt(&Money::amount, decimal_t::from_literal("1")))
          .count() == 1);
}

void test_zero_block_fetch_size(orm& db) {
  // A zero block fetch size made the driver hand back an empty result set
  // without an error, and the row-slot arithmetic below divides by it.
  result_set rs = db.native_connection().execute(
    "SELECT id, name, note FROM uniorm_it_user ORDER BY id", params{}, 0);
  std::size_t rows = 0;
  std::size_t note_chars = 0;
  while (rs.next()) {
    row r = rs.current();
    CHECK(!r.is_null(label("name")));
    if (!r.is_null(label("note"))) {
      note_chars += r.get<std::string>(label("note")).size();
    }
    ++rows;
  }
  CHECK(rows == 3);
  CHECK(note_chars == long_note.size() + 5);  // "short" on the third row
}

void test_projection(orm& db) {
  struct user_row {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
    double balance;
    std::optional<std::string> note;
    std::optional<timestamp> created;
  };

  auto rows = db.query<user_row>(
    "SELECT id, name, age, balance, note, created FROM uniorm_it_user"
    " ORDER BY id");
  CHECK(rows.size() == 3);

  CHECK(rows[0].id == 1 && rows[0].name == "alice");
  CHECK(rows[0].age.has_value() && *rows[0].age == 30);
  CHECK(rows[0].balance == 12.5);
  CHECK(!rows[0].note.has_value());
  CHECK(rows[0].created.has_value() &&
        *rows[0].created == detail::make_timestamp(2024, 1, 2, 3, 4, 5, 0));

  CHECK(!rows[1].age.has_value());
  CHECK(rows[1].note.has_value() && rows[1].note->size() == long_note.size());
  CHECK(!rows[1].created.has_value());

  CHECK(rows[2].balance == -4.25);
  CHECK(rows[2].note.value_or("") == "short");
}

// A registered entity read by a statement the caller wrote pairs each field
// with the column of its name, not the column at its ordinal.
void test_registered_read_by_label(orm& db) {
  auto const rows = db.query<User>(
    "SELECT name, created, id, note, age, balance FROM uniorm_it_user"
    " ORDER BY id");
  CHECK(rows.size() == 3);
  CHECK(rows[0].id == 1 && rows[0].name == "alice");
  CHECK(rows[0].age.has_value() && *rows[0].age == 30);
  CHECK(rows[0].balance == 12.5);
  CHECK(!rows[0].note.has_value());
  CHECK(rows[0].created.has_value() &&
        *rows[0].created == detail::make_timestamp(2024, 1, 2, 3, 4, 5, 0));
  CHECK(!rows[1].age.has_value());
  CHECK(!rows[1].created.has_value());
  CHECK(rows[1].note.has_value() && rows[1].note->size() == long_note.size());
  CHECK(rows[2].balance == -4.25);
  CHECK_THROWS(
    db.query<User>("SELECT id, name FROM uniorm_it_user"), column_not_found);
}

void test_converter_round_trip(orm& db) {
  struct order_projection {
    std::int64_t id;
    grade state;
    std::optional<grade> note;
  };

  std::vector<Order> orders{
    Order{ 1, grade::gold, std::nullopt },
    Order{ 2, grade::silver, grade::bronze },
    Order{ 3, grade::bronze, grade::gold },
  };
  CHECK(db.insert(orders) == 3);

  // What the driver receives is the representation, never the ordinal.
  {
    result_set rs = db.execute(
      "SELECT id, state, note FROM uniorm_it_order WHERE id = ?",
      params{ std::int64_t{ 1 } });
    CHECK(rs.next());
    row r = rs.current();
    CHECK(r.get<std::string>(label("state")) == "gold");
    CHECK(r.get<grade>(label("state")) == grade::gold);
    CHECK(r.is_null(label("note")));
  }

  auto second = db.query<Order>()
                  .where(eq(&Order::id, std::int64_t{ 2 }))
                  .one();
  CHECK(second.has_value());
  CHECK(second->state == grade::silver);
  CHECK(second->note && *second->note == grade::bronze);

  auto listed = db.query<Order>().order_by(&Order::id).all();
  CHECK(listed.size() == 3);
  CHECK(listed[0].state == grade::gold);
  CHECK(!listed[0].note.has_value());  // NULL through the decoding stage
  CHECK(listed[2].note && *listed[2].note == grade::gold);

  auto in_list = db.query<Order>()
                   .where(in(&Order::state, { grade::bronze, grade::silver }))
                   .all();
  CHECK(in_list.size() == 2);

  auto rows = db.query<order_projection>(
    "SELECT id, state, note FROM uniorm_it_order ORDER BY id");
  CHECK(rows.size() == 3);
  CHECK(rows[1].state == grade::silver && rows[1].note == grade::bronze);
  CHECK(!rows[0].note.has_value());

  CHECK(db.update<Order>()
          .where(eq(&Order::id, std::int64_t{ 3 }))
          .set(&Order::state, grade::silver)
          .set(&Order::note, nullptr)
          .execute() == 1);
  auto edited = db.query<Order>()
                  .where(eq(&Order::id, std::int64_t{ 3 }))
                  .one();
  CHECK(edited && edited->state == grade::silver);
  CHECK(edited && !edited->note.has_value());

  // A batch update rewrites every mapped column, so the vector says what the
  // rows hold afterwards -- an absent optional included, which is a NULL. Each
  // row has to differ from the stored one: the tally the driver reports is
  // changed rows, not matched ones.
  orders[0].state = grade::bronze;
  orders[0].note = grade::silver;
  orders[1].note = std::nullopt;
  orders[2].note = grade::bronze;
  CHECK(db.update(orders) == 3);

  auto after = db.query<Order>().order_by(&Order::id).all();
  CHECK(after[0].state == grade::bronze);
  CHECK(after[0].note && *after[0].note == grade::silver);
  CHECK(after[1].state == grade::silver);
  CHECK(!after[1].note.has_value());
  CHECK(after[2].state == grade::bronze);
  CHECK(after[2].note && *after[2].note == grade::bronze);

  CHECK(db.update(k_order_table)
          .set("state", grade::gold)
          .where("id = ?", params{ std::int64_t{ 1 } })
          .execute() == 1);
  CHECK(db.query<Order>()
          .where(eq(&Order::state, grade::gold))
          .count() == 1);

  db.execute_update(std::string("DELETE FROM ") + k_order_table);
}

// One recipe, written through whichever face is holding it: both objects take
// the same map() with the same arguments, so an async session is configured by
// the same lines a synchronous one is.
template <class Session>
void map_entities(Session& db) {
  db.template map<User>(k_table)
    .primary_key("id", &User::id)
    .column("name", &User::name)
    .column("age", &User::age)
    .column("balance", &User::balance)
    .column("note", &User::note)
    .column("created", &User::created);
  db.template map<Pair>(k_pair_table)
    .primary_key("grp", &Pair::grp)
    .primary_key("idx", &Pair::idx)
    .column("label", &Pair::label);
  db.template map<Order>(k_order_table)
    .primary_key("id", &Order::id)
    .column("state", &Order::state)
    .column("note", &Order::note);
  db.template map<Money>(k_money_table)
    .primary_key("id", &Money::id)
    .column("amount", &Money::amount)
    .column("wide", &Money::wide)
    .column("whole", &Money::whole);
}

orm build_registry(std::string_view conn_string) {
  orm db(conn_string);
  map_entities(db);
  return db;
}

void test_validate(std::string_view conn_string) {
  if (!catalog_available()) {
    return;
  }
  orm db = build_registry(conn_string);
  db.validate();  // strict: must pass
  db.validate(validation_mode::lenient);

  struct Bad {
    std::int64_t id = 0;
  };
  {
    orm bad(conn_string);
    bad.map<Bad>(k_table).primary_key("no_such_col", &Bad::id);
    CHECK_THROWS(bad.validate(), mapping_error);
  }
  {
    orm bad(conn_string);
    bad.map<Bad>("no_such_table").primary_key("id", &Bad::id);
    CHECK_THROWS(bad.validate(), mapping_error);
  }
  {
    struct NoOpt {
      std::int64_t id = 0;
      std::int32_t age = 0;  // non-optional against nullable column
    };
    orm strict_reg(conn_string);
    strict_reg.map<NoOpt>(k_table)
      .primary_key("id", &NoOpt::id)
      .column("age", &NoOpt::age);
    CHECK_THROWS(strict_reg.validate(), mapping_error);
    try {
      strict_reg.validate(validation_mode::lenient);
      CHECK(true);
    } catch (...) {
      CHECK(false);
    }
  }
  {
    // The family comparison keys on what the member binds, which for a
    // converter type is its representation, not the domain type.
    struct Swapped {
      grade id = grade::bronze;  // text against a BIGINT column
      std::int64_t state = 0;    // bigint against a VARCHAR column
    };
    orm bad(conn_string);
    bad.map<Swapped>(k_order_table)
      .primary_key("id", &Swapped::id)
      .column("state", &Swapped::state);
    CHECK_THROWS(bad.validate(), mapping_error);
    try {
      bad.validate(validation_mode::lenient);
      CHECK(true);
    } catch (...) {
      CHECK(false);
    }
  }
}

void test_identifier_spelling(std::string_view conn_string) {
  if (!catalog_available()) {
    return;
  }
  orm probe(conn_string);
  auto& metadata = probe.schema();
  std::string catalog_name = metadata.database_name();
  std::string schema_name;
  if (probe.native_connection().sql_dialect().table_qualification ==
      dialect::qualification::schema) {
    auto result = probe.execute("SELECT current_schema()");
    CHECK(result.next());
    schema_name = result.current().get<std::string>(0);
  }
  std::string const lowered = detail::fold_lower(k_case_table);
  std::string stored;
  for (auto const& table : metadata.exact_tables(catalog_name, schema_name)) {
    if (detail::fold_lower(table.name) == lowered) {
      stored = table.name;
      break;
    }
  }
  CHECK(!stored.empty());
  if (stored.empty()) {
    return;
  }

  std::string const declared = stored == lowered ? k_case_table : lowered;

  // Declared opposite to the way this server reports a name, so whichever way
  // it folds, the mapping starts out disagreeing with the catalog.
  auto opposite = [](std::string const& name) {
    return name == detail::fold_lower(name) ? detail::fold_upper(name)
                                            : detail::fold_lower(name);
  };
  std::string reported_id;
  std::string id_column;
  std::string name_column;
  for (auto const& column : metadata.exact_table_columns(
         { catalog_name, schema_name, stored })) {
    std::string const folded = detail::fold_lower(column.shape.name);
    if (folded == "id") {
      reported_id = column.shape.name;
      id_column = opposite(column.shape.name);
    } else if (folded == "name") {
      name_column = opposite(column.shape.name);
    }
  }
  CHECK(!reported_id.empty() && !id_column.empty() && !name_column.empty());
  if (id_column.empty() || name_column.empty()) {
    return;
  }

  // The fold carrying one spelling onto the other; no answer where the two
  // differ by more than case.
  auto fold_to = [](std::string const& from, std::string const& to)
    -> std::optional<dialect::identifier_case> {
    if (detail::fold_upper(from) == to) {
      return dialect::identifier_case::upper;
    }
    if (detail::fold_lower(from) == to) {
      return dialect::identifier_case::lower;
    }
    return std::nullopt;
  };
  // How a server folds its table names and how it folds its column names are
  // two facts, and one policy has to serve both: that is what this records.
  std::printf("note: %s stores %s as %s and %s as %s\n",
    probe.native_connection().dbms_name().c_str(), declared.c_str(),
    stored.c_str(), id_column.c_str(), reported_id.c_str());
  auto const table_fold = fold_to(declared, stored);
  auto const column_fold = fold_to(id_column, reported_id);
  CHECK(table_fold.has_value());
  CHECK(column_fold.has_value());

  orm db(conn_string);
  db.map<CaseUser>(declared)
    .primary_key(id_column, &CaseUser::id)
    .column(name_column, &CaseUser::name);

  // A server that folds the names it is given answers the undeclared mapping
  // as well: the policy comes off the banner, so nothing has to be declared
  // for the spelling this server stores to be the one the mapping writes.
  if (the_target.server_dialect.identifiers !=
      dialect::identifier_case::keep) {
    db.validate();
    CHECK(db.query<CaseUser>().count() == 1);
  }

  // `keep` has to miss on every server, and miss on the table name. Asked of
  // the policy that folds nothing rather than of the dialect's default, which
  // on a server that folds its own names would carry this one onto the stored
  // spelling and hit.
  db.identifier_case(dialect::identifier_case::keep);
  std::string miss;
  try {
    db.validate();
  } catch (mapping_error const& e) {
    miss = e.what();
  }
  CHECK(!miss.empty());
  if (miss.empty()) {
    return;
  }
  // One mistake, one message on every server: a catalog answering a name
  // whatever its case is stopped at the table rather than at the column it
  // happens to report. The candidate carries the server's own schema
  // qualification, so the stored spelling is pinned, not the whole message.
  CHECK(miss.find("table not found: " + declared) != std::string::npos);
  CHECK(miss.find("only case differs from") != std::string::npos);
  CHECK(miss.find(stored) != std::string::npos);

  if (table_fold && table_fold == column_fold) {
    db.identifier_case(*table_fold);
    db.validate();
    CHECK(db.query<CaseUser>().count() == 1);
  } else if (table_fold) {
    db.identifier_case(*table_fold);
    CHECK_THROWS(db.validate(), mapping_error);
  }

  db.resolve_identifiers(catalog_name, schema_name);
  db.identifier_case(dialect::identifier_case::upper);
  db.validate();
  CHECK(db.meta<CaseUser>().table == declared);
  CHECK(db.meta<CaseUser>().columns[0].column == id_column);
  auto rows = db.query<CaseUser>().all();
  CHECK(rows.size() == 1);
  if (rows.size() == 1) {
    CHECK(rows[0].id == 1);
    CHECK(rows[0].name == "raised");
  }

  auto transaction = db.begin();
  std::vector<CaseUser> added{ { 2, "two" }, { 3, "three" } };
  CHECK(db.insert(added) == 2);
  added[0].name = "second";
  CHECK(db.update(added[0], { id_column }) == 1);
  // MySQL and MariaDB answer an UPDATE with rows changed, so the batch below
  // has to alter every row it names or the driver will only count one.
  added[0].name = "second again";
  added[1].name = "third";
  CHECK(db.update(added) == 2);
  CHECK(db.update<CaseUser>().where(eq(&CaseUser::id, std::int64_t{ 3 }))
    .set(&CaseUser::name, "changed").execute() == 1);
  auto selected = db.query<CaseUser>()
    .where(ge(&CaseUser::id, std::int64_t{ 2 }))
    .order_by(&CaseUser::id).all();
  CHECK(selected.size() == 2);
  if (selected.size() == 2) {
    CHECK(selected[0].name == "second again");
    CHECK(selected[1].name == "changed");
  }
  CHECK(db.remove(added[0], { id_column }) == 1);
  CHECK(db.remove(std::vector<CaseUser>{ added[1] }) == 1);
  CHECK(db.insert(std::vector<CaseUser>{ { 4, "four" } }) == 1);
  CHECK(db.remove<CaseUser>().where(eq(&CaseUser::id, std::int64_t{ 4 }))
    .execute() == 1);
  CHECK(db.query<CaseUser>().count() == 1);
  transaction.rollback();
}

void test_identifier_case_twins(std::string_view conn_string) {
  if (!catalog_available()) {
    return;
  }
  orm db(conn_string);
  auto const& dialect = db.native_connection().sql_dialect();
  if (dialect.table_qualification != dialect::qualification::schema) {
    return;
  }
  std::string const catalog_name = db.schema().database_name();
  std::string schema_name;
  {
    auto schema_result = db.execute("SELECT current_schema()");
    CHECK(schema_result.next());
    schema_name = schema_result.current().get<std::string>(0);
  }
  std::string const prefix = dialect.quote_exact_identifier(schema_name) + ".";
  std::string const mixed_name = "Uniorm_IT_Resolve_Twin";
  std::string const lower_name = detail::fold_lower(mixed_name);
  std::string const mixed_table = prefix +
    dialect.quote_exact_identifier(mixed_name);
  std::string const lower_table = prefix +
    dialect.quote_exact_identifier(lower_name);
  auto transaction = db.begin();
  db.execute_update("CREATE TABLE " + lower_table +
    " (id BIGINT NOT NULL PRIMARY KEY, name VARCHAR(64) NOT NULL)");
  db.execute_update("INSERT INTO " + lower_table + " VALUES (22, 'lower')");
  db.map<CaseUser>(mixed_name)
    .primary_key("Id", &CaseUser::id).column("Name", &CaseUser::name);
  db.resolve_identifiers(catalog_name, schema_name);
  CHECK(db.meta<CaseUser>().resolved->table.name == lower_name);
  auto first = db.query<CaseUser>().one();
  CHECK(first && first->id == 22 && first->name == "lower");

  db.execute_update("CREATE TABLE " + mixed_table +
    " (\"ID\" BIGINT NOT NULL PRIMARY KEY, \"NAME\" VARCHAR(64) NOT NULL,"
    " \"EXTRA\" INT)");
  db.execute_update("INSERT INTO " + mixed_table +
    " (\"ID\", \"NAME\") VALUES (11, 'exact')");
  db.resolve_identifiers(catalog_name, schema_name);
  CHECK(db.meta<CaseUser>().resolved->table.name == mixed_name);
  CHECK(db.meta<CaseUser>().resolved->columns[0] == "ID");
  db.validate();
  auto exact = db.query<CaseUser>().one();
  CHECK(exact && exact->id == 11 && exact->name == "exact");

  // The other reading of the same two names, without a catalog in the way: a
  // fold asks for the twin it lands on, columns and all, so where a schema
  // holds both spellings a policy is the wrong tool and resolution above is
  // the right one.
  db.clear_identifier_resolution();
  db.identifier_case(dialect::identifier_case::lower);
  auto folded = db.query<CaseUser>().one();
  CHECK(folded && folded->id == 22 && folded->name == "lower");
  db.identifier_case(dialect::identifier_case::keep);
  CHECK(db.query<CaseUser>().build_select().find(
    dialect.quote_exact_identifier(mixed_name)) != std::string::npos);
  db.identifier_case(std::nullopt);

  db.execute_update("CREATE TEMP TABLE " +
    dialect.quote_exact_identifier(mixed_name) +
    " (\"ID\" BIGINT, \"NAME\" VARCHAR(64))");
  db.execute_update("INSERT INTO " + dialect.quote_exact_identifier(mixed_name) +
    " VALUES (99, 'shadow')");
  db.resolve_identifiers(catalog_name, schema_name);
  auto qualified = db.query<CaseUser>().one();
  CHECK(qualified && qualified->id == 11 && qualified->name == "exact");

  struct AmbiguousUser { std::int64_t id; };
  db.map<AmbiguousUser>(detail::fold_upper(mixed_name))
    .primary_key("ID", &AmbiguousUser::id);
  CHECK_THROWS(db.resolve_identifiers(catalog_name, schema_name), mapping_error);
  CHECK(!db.meta<AmbiguousUser>().resolved);
  CHECK(db.meta<CaseUser>().resolved->table.name == mixed_name);
  transaction.rollback();
}

void test_query_builder(orm& db) {
  CHECK(db.query<User>().count() == 3);

  auto all = db.query<User>().all();
  CHECK(all.size() == 3);

  auto one = db.query<User>()
               .where(eq(&User::name, std::string("alice")))
               .one();
  CHECK(one.has_value() && one->id == 1 && one->age.value_or(0) == 30);
  CHECK(one->created.has_value());

  auto none = db.query<User>()
                .where(eq(&User::name, std::string("nobody")))
                .one();
  CHECK(!none.has_value());

  auto adults = db.query<User>().where(gt(&User::age, 26)).all();
  CHECK(adults.size() == 1 && adults[0].id == 1);  // NULL age excluded

  auto nulls = db.query<User>().where(is_null(&User::age)).all();
  CHECK(nulls.size() == 1 && nulls[0].id == 2);

  auto picked =
    db.query<User>()
      .where(in(&User::id, { std::int64_t{ 1 }, std::int64_t{ 3 } }))
      .all();
  CHECK(picked.size() == 2);

  auto liked =
    db.query<User>().where(like(&User::name, "a%")).all();
  CHECK(liked.size() == 1 && liked[0].name == "alice");

  auto combined =
    db.query<User>()
      .where(gt(&User::age, 20) && ne(&User::name, std::string("carol")))
      .all();
  CHECK(combined.size() == 1 && combined[0].id == 1);

  auto page = db.query<User>()
                .order_by(&User::id, direction::desc)
                .limit(2)
                .offset(1)
                .all();
  CHECK(page.size() == 2 && page[0].id == 2 && page[1].id == 1);

  std::string sql = db.query<User>()
                      .where(eq(&User::id, std::int64_t{ 1 }))
                      .build_select();
  // Which quote character lands here is the server's own doing: the dialect is
  // detected from the banner this connection read, not from the driver.
  dialect const d = dialect::detect(db.native_connection().dbms_name());
  CHECK(sql.find(d.quote_identifier("uniorm_it_user")) != std::string::npos);
  CHECK(sql.find(d.quote_identifier("id") + " = ?") != std::string::npos);

  // The spelling policy rides the same emission point, so one mapping can
  // serve a server that stores its names the other way. The expectation is
  // folded by that policy rather than by the banner's own answer, which on a
  // server that folds is a different one.
  db.identifier_case(dialect::identifier_case::upper);
  dialect raised_dialect = d;
  raised_dialect.identifiers = dialect::identifier_case::upper;
  std::string raised = db.query<User>()
                         .where(eq(&User::id, std::int64_t{ 1 }))
                         .build_select();
  CHECK(raised.find(
    raised_dialect.quote_identifier("UNIORM_IT_USER")) != std::string::npos);
  // What a fold removes is the declared spelling, not the folded one: on a
  // server whose dialect already folds, the two are the same text.
  std::string const declared_spelling =
    d.quote_exact_identifier("id") + " = ?";
  CHECK(raised.find(declared_spelling) == std::string::npos);
  // Undeclare rather than declare keep: keep is a policy of its own, and the
  // stages after this one run on this same object.
  db.identifier_case(std::nullopt);
}

void test_transaction(orm& db) {
  {
    transaction tx = db.begin();
    CHECK(tx.active());
    db.execute_update("INSERT INTO uniorm_it_user (id, name, age, balance)"
                        " VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 100 }, std::string("temp"), nullptr, 0.0 });
    tx.rollback();
    CHECK(!tx.active());
  }
  CHECK(db.query<User>().count() == 3);

  {
    transaction tx = db.begin();
    db.execute_update("INSERT INTO uniorm_it_user (id, name, age, balance)"
                        " VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 101 }, std::string("kept"), nullptr, 0.0 });
    tx.commit();
  }
  CHECK(db.query<User>().count() == 4);

  {
    transaction tx = db.begin();  // destructor must roll back
    db.execute_update("INSERT INTO uniorm_it_user (id, name, age, balance)"
                        " VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 102 }, std::string("dropped"), nullptr, 0.0 });
  }
  CHECK(db.query<User>().count() == 4);

  db.execute_update(
    "DELETE FROM uniorm_it_user WHERE id = ?", params{ std::int64_t{ 101 } });
  CHECK(db.query<User>().count() == 3);
}

// auto_commit is the connection's commit mode, so it governs every write here:
// off, nothing is durable until commit(); on, each statement is.
void test_auto_commit_scope(orm& db, std::string const& conn_string) {
  // A run that died inside this section leaves 700 and up behind, and every
  // count below is over that range.
  db.execute_update("DELETE FROM uniorm_it_user WHERE id >= 700");
  CHECK(db.auto_commit());
  auto mine = [&db] {
    return db.query<User>()
      .where(ge(&User::id, std::int64_t{ 700 }))
      .count();
  };
  auto visible = std::string(
    "SELECT COUNT(*) FROM uniorm_it_user WHERE id >= 700");
  auto seen_elsewhere = [&](orm& other) {
    result_set rs = other.execute(visible);
    rs.next();
    return rs.current().get<std::int64_t>(0);
  };
  auto put = [](orm& db, std::int64_t id) {
    db.execute_update(
      "INSERT INTO uniorm_it_user (id, name, age, balance) VALUES (?, ?, ?, ?)",
      params{ id, std::string("leaked"), nullptr, 0.0 });
  };
  std::vector<User> batch{
    User{ 700, "henry", std::nullopt, 0.0, std::nullopt, std::nullopt },
    User{ 701, "iris", std::nullopt, 0.0, std::nullopt, std::nullopt }
  };

  // A caller transaction covers all three write shapes: rolling it back must
  // erase the batch too, which it used not to.
  {
    transaction tx = db.begin();
    CHECK(db.insert(batch) == 2);
    User renamed = batch[1];
    renamed.name = "rita";
    CHECK(db.update(renamed) == 1);
    db.execute_update(
      "INSERT INTO uniorm_it_user (id, name, age, balance) VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 702 }, std::string("june"), nullptr, 0.0 });
    CHECK(mine() == 3);  // all three visible inside the transaction
    tx.rollback();
  }
  CHECK(mine() == 0);
  CHECK(db.native_connection().autocommit());

  // Same shape committed.
  {
    transaction tx = db.begin();
    CHECK(db.insert(batch) == 2);
    tx.commit();
  }
  CHECK(mine() == 2);
  CHECK(db.remove(batch) == 2);
  CHECK(mine() == 0);

  // Autocommitting, single-row and batch writes are durable on return.
  CHECK(db.insert(batch) == 2);
  CHECK(mine() == 2);
  CHECK(db.remove(batch) == 2);
  CHECK(mine() == 0);

  // Manual mode defers every kind of write, not just the batch one.
  db.auto_commit(false);
  CHECK(!db.auto_commit());
  CHECK(!db.native_connection().autocommit());
  orm other(conn_string);
  CHECK(db.insert(batch) == 2);
  User renamed = batch[0];
  renamed.name = "kate";
  CHECK(db.update(renamed) == 1);
  CHECK(mine() == 2);        // this connection sees its own pending work
  CHECK(seen_elsewhere(other) == 0);
  db.commit();
  CHECK(seen_elsewhere(other) == 2);  // durable at last

  // A rollback discards a sweep and a single-row delete alike.
  CHECK(db.remove(batch) == 2);
  CHECK(mine() == 0);
  CHECK(seen_elsewhere(other) == 2);
  db.rollback();
  CHECK(mine() == 2);
  CHECK(seen_elsewhere(other) == 2);

  // Switching the mode back on does not discard: it commits what is pending.
  std::vector<User> extra{
    User{ 702, "june", std::nullopt, 0.0, std::nullopt, std::nullopt }
  };
  CHECK(db.insert(extra) == 1);
  CHECK(mine() == 3);
  CHECK(seen_elsewhere(other) == 2);
  db.auto_commit(true);
  CHECK(seen_elsewhere(other) == 3);
  CHECK(db.remove(extra) == 1);
  CHECK(mine() == 2);
  db.execute_update("DELETE FROM uniorm_it_user WHERE id >= 700");
  CHECK(mine() == 0);

  // A lease that ends in manual mode leaves the next borrower mid-transaction;
  // those rows are discarded, whichever mode the borrower wants.
  {
    orm leaker(conn_string);
    leaker.auto_commit(false);
    put(leaker, 703);
  }
  {
    orm fresh(conn_string);
    CHECK(fresh.native_connection().autocommit());
    put(fresh, 704);
    CHECK(mine() == 1);  // 704 durable, 703 gone
  }
  {
    orm leaker(conn_string);
    leaker.auto_commit(false);
    put(leaker, 705);
  }
  {
    orm manual;
    manual.auto_commit(false);
    manual.connect(conn_string);
    CHECK(!manual.native_connection().autocommit());
    put(manual, 706);
    CHECK(seen_elsewhere(manual) == 2);  // 704 durable, plus its own 706
    CHECK(seen_elsewhere(other) == 1);   // just 704 so far
    manual.commit();
    CHECK(seen_elsewhere(other) == 2);
  }

  db.execute_update("DELETE FROM uniorm_it_user WHERE id >= 700");
  CHECK(mine() == 0);
}

void test_insert(orm& db) {
  std::vector<User> users;
  users.push_back(User{ 200, "dave", std::int32_t{ 40 }, 1.5,
    std::string("batched"), std::nullopt });
  users.push_back(User{ 201, "erin", std::nullopt, 0.0, std::nullopt,
    detail::make_timestamp(2025, 6, 7, 8, 9, 10, 0) });
  users.push_back(
    User{ 202, "frank", std::int32_t{ 51 }, -2.0, std::nullopt, std::nullopt });

  CHECK(db.insert(users) == 3);
  CHECK(db.query<User>().count() == 6);

  auto erin = db.query<User>()
                .where(eq(&User::id, std::int64_t{ 201 }))
                .one();
  CHECK(erin.has_value());
  CHECK(erin->name == "erin");
  CHECK(!erin->age.has_value());  // empty optional written as NULL
  CHECK(!erin->note.has_value());
  CHECK(erin->created.has_value());

  // 1500 rows x 6 columns exceeds the per-statement placeholder cap,
  // forcing multiple multi-row VALUES statements in one transaction.
  std::vector<User> many;
  many.reserve(1500);
  for (std::int64_t id = 1000; id < 2500; ++id) {
    many.push_back(
      User{ id, "bulk", std::nullopt, 0.0, std::nullopt, std::nullopt });
  }
  CHECK(db.insert(many) == 1500);
  CHECK(db.query<User>().count() == 6 + 1500);

  db.execute_update(
    "DELETE FROM uniorm_it_user WHERE id >= ?", params{ std::int64_t{ 200 } });
  CHECK(db.query<User>().count() == 3);
}

void test_update(orm& db) {
  std::vector<User> users{
    User{ 400, "ivy", std::int32_t{ 33 }, 1.0, std::nullopt, std::nullopt },
    User{ 401, "jack", std::nullopt, 2.0, std::string("orig"), std::nullopt }
  };
  db.insert(users);
  CHECK(db.query<User>().count() == 5);

  // Dynamic update: bound values, including a NULL write.
  CHECK(db.update(k_table)
          .set("age", std::int32_t{ 34 })
          .set("note", nullptr)
          .where("id = ?", params{ std::int64_t{ 400 } })
          .execute() == 1);
  auto ivy = db.query<User>()
               .where(eq(&User::id, std::int64_t{ 400 }))
               .one();
  CHECK(ivy.has_value() && ivy->age.value_or(0) == 34);
  CHECK(!ivy->note.has_value());

  // Entity update through the builder.
  CHECK(db.update<User>()
          .where(eq(&User::id, std::int64_t{ 401 }))
          .set(&User::note, std::string("edited"))
          .set(&User::balance, 2.5)
          .execute() == 1);
  auto jack = db.query<User>()
                .where(eq(&User::id, std::int64_t{ 401 }))
                .one();
  CHECK(jack.has_value() && jack->note.value_or("") == "edited");
  CHECK(jack->balance == 2.5);

  // Guards: empty set or blank where must throw.
  CHECK_THROWS(db.update(k_table)
                 .where("id = ?", params{ std::int64_t{ 400 } })
                 .execute(),
    uniorm_error);
  CHECK_THROWS(
    db.update(k_table).set("age", std::int32_t{ 1 }).execute(), uniorm_error);
  CHECK_THROWS(
    db.update<User>().where(eq(&User::id, std::int64_t{ 400 })).execute(),
    uniorm_error);
  CHECK_THROWS(
    db.update<User>().set(&User::age, std::int32_t{ 1 }).execute(),
    uniorm_error);

  db.execute_update(
    "DELETE FROM uniorm_it_user WHERE id >= ?", params{ std::int64_t{ 400 } });
  CHECK(db.query<User>().count() == 3);
}

void test_remove(orm& db) {
  std::vector<User> users{
    User{ 400, "ivy", std::int32_t{ 33 }, 1.0, std::nullopt, std::nullopt },
    User{ 401, "jack", std::nullopt, 2.0, std::nullopt, std::nullopt }
  };
  db.insert(users);
  CHECK(db.query<User>().count() == 5);

  // Entity delete through the builder.
  CHECK(db.remove<User>()
          .where(eq(&User::id, std::int64_t{ 400 }))
          .execute() == 1);
  CHECK(db.query<User>().count() == 4);

  // Dynamic delete.
  CHECK(db.remove(k_table)
          .where("id = ?", params{ std::int64_t{ 401 } })
          .execute() == 1);
  CHECK(db.query<User>().count() == 3);

  // Guards: blank where must throw.
  CHECK_THROWS(db.remove(k_table).execute(), uniorm_error);
  CHECK_THROWS(db.remove<User>().execute(), uniorm_error);
}

void test_entity_update(orm& db) {
  // Insert a test row.
  User u{ 500, "eve", std::int32_t{ 28 }, 10.0, std::string("original"),
    std::nullopt };
  db.insert(std::vector<User>{ u });
  CHECK(db.query<User>().count() == 4);

  // Update using primary key as WHERE.
  User u2{ 500, "eve_updated", std::int32_t{ 29 }, 11.5, std::string("modified"),
    std::nullopt };
  CHECK(db.update(u2) == 1);
  auto row = db.query<User>()
               .where(eq(&User::id, std::int64_t{ 500 }))
               .one();
  CHECK(row.has_value());
  CHECK(row->name == "eve_updated");
  CHECK(row->age.value_or(0) == 29);
  CHECK(row->balance == 11.5);
  CHECK(row->note.value_or("") == "modified");

  // Update using specified field as WHERE.
  User u3{ 501, "eve_updated", std::int32_t{ 30 }, 12.0, std::string("again"),
    std::nullopt };
  CHECK(db.update(u3, { "name" }) == 1);
  auto row2 = db.query<User>()
                .where(eq(&User::name, std::string("eve_updated")))
                .one();
  CHECK(row2.has_value());
  CHECK(row2->id == 501);
  CHECK(row2->age.value_or(0) == 30);

  // Guards.
  CHECK_THROWS(db.update(u, {}), uniorm_error);
  CHECK_THROWS(db.update(u, { "nonexistent" }), uniorm_error);

  // Cleanup.
  db.execute_update(
    "DELETE FROM uniorm_it_user WHERE id >= ?", params{ std::int64_t{ 500 } });
  CHECK(db.query<User>().count() == 3);
}

void test_composite_key_and_where_fields(orm& db) {
  std::vector<Pair> pairs{ { 7, 1, "a" }, { 7, 2, "b" }, { 8, 1, "c" } };
  CHECK(db.insert(pairs) == 3);

  auto label_of = [&db](std::int64_t grp, std::int64_t idx) {
    std::string sql =
      std::string("SELECT label FROM ") + k_pair_table +
      " WHERE grp = ? AND idx = ?";
    result_set rs = db.execute(sql, params{ grp, idx });
    if (!rs.next()) {
      return std::string("<missing>");
    }
    return rs.current().get<std::string>(label("label"));
  };

  // Keying on the leading column alone would have rewritten every row of
  // group 7.
  Pair updated{ 7, 2, "b2" };
  CHECK(db.update(updated) == 1);
  CHECK(label_of(7, 2) == "b2");
  CHECK(label_of(7, 1) == "a");

  std::vector<Pair> rebased{ { 8, 1, "c2" } };
  CHECK(db.update(rebased) == 1);
  CHECK(label_of(8, 1) == "c2");
  CHECK(label_of(7, 1) == "a");

  CHECK(db.remove(Pair{ 7, 1, "a" }) == 1);
  CHECK(label_of(7, 1) == "<missing>");
  CHECK(label_of(7, 2) == "b2");

  std::vector<Pair> doomed{ { 7, 2, "b2" }, { 8, 1, "c2" } };
  CHECK(db.remove(doomed) == 2);
  CHECK(label_of(7, 2) == "<missing>");
  CHECK(label_of(8, 1) == "<missing>");

  // Unmapped WHERE fields used to be dropped on the way to the statement,
  // leaving placeholders without parameters and a misleading driver error.
  std::vector<User> probe{ User{ 1, "alice", std::nullopt, 0.0, std::nullopt,
    std::nullopt } };
  auto const partly_unknown =
    std::vector<std::string>{ "id", "colour" };
  CHECK_THROWS(db.update(probe, partly_unknown), mapping_error);
  CHECK_THROWS(db.remove(probe, partly_unknown), mapping_error);
  auto const none = std::vector<std::string>{};
  CHECK_THROWS(db.update(probe, none), uniorm_error);
  CHECK_THROWS(db.remove(probe, none), uniorm_error);
  // A WHERE covering every mapped column leaves nothing to assign.
  auto const everything =
    std::vector<std::string>{ "id", "name", "age", "balance", "note",
      "created" };
  CHECK_THROWS(db.update(probe, everything), uniorm_error);
}

void test_statement_cache(orm& db) {
  char const* select_sql = "SELECT id, name FROM uniorm_it_user ORDER BY id";

  // Streaming result_set: checked out until destroyed, so a second
  // execute of the same SQL while the first is open is a miss.
  unsigned long long hits = db.statement_cache_hits();
  unsigned long long misses = db.statement_cache_misses();
  {
    result_set rs1 = db.execute(select_sql);
    result_set rs2 = db.execute(select_sql);
    CHECK(db.statement_cache_hits() == hits);
    CHECK(db.statement_cache_misses() == misses + 2);
    CHECK(rs1.next() && rs2.next());
  }  // both returned to the cache here

  // Same SQL again: reuse the prepared statement, identical rows.
  {
    result_set rs = db.execute(select_sql);
    CHECK(db.statement_cache_hits() == hits + 1);
    std::vector<std::string> names;
    while (rs.next()) {
      names.push_back(rs.current().get<std::string>(label("name")));
    }
    CHECK(names.size() == 3);
    CHECK(names[0] == "alice" && names[1] == "bob" && names[2] == "carol");
  }

  // execute_update path returns the statement immediately.
  char const* noop_update = "DELETE FROM uniorm_it_user WHERE id < ?";
  db.execute_update(noop_update, params{ std::int64_t{ 0 } });
  hits = db.statement_cache_hits();
  CHECK(db.execute_update(noop_update, params{ std::int64_t{ 0 } }) == 0);
  CHECK(db.statement_cache_hits() == hits + 1);

  // Entity queries participate too. Clear the cache first so the first
  // call below is a guaranteed miss.
  db.clear_statement_cache();
  CHECK(db.statement_cache_size() == 0);
  hits = db.statement_cache_hits();
  CHECK(db.query<User>().all().size() == 3);
  CHECK(db.query<User>().all().size() == 3);
  CHECK(db.statement_cache_hits() == hits + 1);
  CHECK(db.statement_cache_size() > 0);
}

// A parse the session made can be gone while the statement still counts on it:
// the name is this plugin's own, and the answer that says the server holds no
// such name can only be read by making the parse again. DEALLOCATE is how a
// run takes one away on purpose.
void test_a_dropped_parse_is_made_again(orm& db) {
  if (!the_target.holds_its_parses_by_name) {
    skip_reason = "this backend holds no parse for the server to give back";
    return;
  }
  char const* sql = "SELECT id, name FROM uniorm_it_user ORDER BY id";
  {
    result_set rs = db.execute(sql);
    CHECK(rs.next());
  }  // the cache keeps the parse the server is about to lose
  db.execute_update("DEALLOCATE ALL");
  std::size_t read = 0;
  result_set again = db.execute(sql);
  while (again.next()) {
    ++read;
  }
  CHECK(read == 3);
  CHECK(db.statement_cache_hits() > 0);
}

void test_error_reporting(orm& db) {
  // A driver failure surfaces as a backend_error: the layer below throws its
  // own type, which derives from it, so callers never need the private header
  // to classify SQL errors or read the code the driver reported.
  bool reported = false;
  try {
    db.execute("SELECT * FROM uniorm_it_no_such_table");
  } catch (backend::backend_error const& e) {
    reported = true;
    CHECK(e.backend_name() == the_target.scheme);
    CHECK(!e.diagnostics().empty());
    if (!e.diagnostics().empty()) {
      auto const& first = e.diagnostics()[0];
      CHECK(state_is_well_formed(first.state));
      CHECK(!first.message.empty());
      CHECK(std::string(e.what()).find("[" + first.state + "]") !=
            std::string::npos);
    }
  }
  CHECK(reported);
}

void test_pool(std::string const& conn_string) {
  pool_options opts;
  opts.target = conn_string;
  opts.size = 2;
  opts.acquire_timeout = std::chrono::milliseconds(1000);
  connection_pool pool(std::move(opts));
  CHECK(pool.capacity() == 2);

  {
    pooled_connection a = pool.acquire();
    pooled_connection b = pool.acquire();
    CHECK(bool(a) && bool(b));
    CHECK(a.is_open() && b.is_open());
    CHECK_THROWS(pool.acquire(), pool_timeout);  // exhausted
  }

  pooled_connection c = pool.acquire();  // returned by destructors above
  CHECK(bool(c));
  CHECK(c.is_open());
}

// Only a maintenance pass retires a connection, so an emptied idle list is a
// state to wait for rather than a value to sample once.
bool idle_empties(connection_pool const& pool) {
  for (int i = 0; i < 200; ++i) {
    if (pool.idle_count() == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

void test_pool_maintenance(std::string const& conn_string) {
  using namespace std::chrono_literals;
  {
    pool_options opts;
    opts.target = conn_string;
    opts.size = 1;
    opts.acquire_timeout = 1000ms;
    opts.heartbeat_interval = 50ms;
    opts.max_idle_time = 2000ms;
    connection_pool pool(std::move(opts));

    {
      pooled_connection c = pool.acquire();
      CHECK(c.is_open());
    }
    CHECK(pool.idle_count() == 1);
    std::this_thread::sleep_for(300ms);
    CHECK(pool.heartbeats_executed() >= 1);  // maintainer ran the heartbeat
    CHECK(pool.idle_count() == 1);  // and kept the connection alive
    // The connection being heartbeated is still in the pool, so the count must
    // hold at 1; a single sample lands inside a pass ~1 time in 75.
    bool steady = true;
    auto until = std::chrono::steady_clock::now() + 200ms;
    while (std::chrono::steady_clock::now() < until) {
      steady = steady && pool.idle_count() != 0;
    }
    CHECK(steady);
    std::this_thread::sleep_for(2500ms);  // now idle beyond max_idle_time
    CHECK(idle_empties(pool));

    pooled_connection again = pool.acquire();  // lazily recreated
    CHECK(bool(again));
    CHECK(again.is_open());
  }
  {
    // A failing heartbeat discards the connection well before max idle.
    pool_options opts;
    opts.target = conn_string;
    opts.size = 1;
    opts.acquire_timeout = 1000ms;
    opts.heartbeat_interval = 50ms;
    opts.max_idle_time = 10000ms;
    opts.heartbeat_sql = "THIS STATEMENT IS NOT VALID SQL";
    connection_pool pool(std::move(opts));

    {
      pooled_connection c = pool.acquire();
      CHECK(bool(c));
    }
    CHECK(pool.idle_count() == 1);
    std::this_thread::sleep_for(300ms);
    CHECK(idle_empties(pool));
  }
  {
    // Two live pools are serviced by the same global scheduler thread.
    pool_options opts;
    opts.target = conn_string;
    opts.size = 1;
    opts.acquire_timeout = 1000ms;
    opts.heartbeat_interval = 50ms;
    opts.max_idle_time = 5000ms;
    connection_pool a(opts);
    connection_pool b(opts);
    {
      pooled_connection ca = a.acquire();
      pooled_connection cb = b.acquire();
      CHECK(bool(ca) && bool(cb));
    }
    std::this_thread::sleep_for(300ms);
    CHECK(a.heartbeats_executed() >= 1);
    CHECK(b.heartbeats_executed() >= 1);
    CHECK(a.idle_count() == 1);
    CHECK(b.idle_count() == 1);
  }
  {
    // The count must hold at n while the maintainer publishes its verdicts one
    // at a time: a gap between credit and entry shows up as a dip here.
    constexpr std::size_t n = 4;
    pool_options opts;
    opts.target = conn_string;
    opts.size = n;
    opts.acquire_timeout = 1000ms;
    opts.heartbeat_interval = 20ms;  // so the sample crosses many passes
    opts.max_idle_time = 10000ms;    // only a failed heartbeat may drop one
    connection_pool pool(std::move(opts));
    {
      std::vector<pooled_connection> held;
      for (std::size_t i = 0; i < n; ++i) {
        held.push_back(pool.acquire());
      }
    }
    bool intact = true;
    auto until = std::chrono::steady_clock::now() + 300ms;
    while (std::chrono::steady_clock::now() < until) {
      intact = intact && pool.idle_count() == n;
    }
    CHECK(intact);
  }
}

// The four pieces have to reach the server the hand-written string reaches,
// with the backend doing the spelling.
void test_connection_pieces(
  std::string const& dsn, std::string const& user, std::string const& pwd) {
  std::string const spelled = the_target.connect_string;
  connection by_string(spelled);
  connection_spec spec{ the_target.scheme, dsn, user, pwd };
  connection by_pieces(spec);
  CHECK(by_pieces.dbms_name() == by_string.dbms_name());
  result_set opened = by_pieces.execute("SELECT 1");
  CHECK(opened.next());
  CHECK(!opened.next());

  // The catalog is read over that same open, so the pieces carry an
  // introspection too: the fixture table this run created has to show up.
  if (the_target.answers_catalog) {
    auto& metadata = by_pieces.schema();
    bool fixture_seen = false;
    for (auto const& table : metadata.tables("", "")) {
      fixture_seen = fixture_seen || table.name == label(k_table);
    }
    CHECK(fixture_seen);
  }

  // orm's spec form leases from a spec-keyed pool, which is a pool of its
  // own next to the string route's.
  {
    orm leased(spec);
    result_set again = leased.execute("SELECT 1");
    CHECK(again.next());
  }

  // An orm already connected by the string route can be moved onto the
  // pieces; the lease is dropped first, as the two routes hold separate pools
  // and each may be small.
  {
    orm moved(spelled);
    moved.disconnect();
    moved.connect(spec);
    result_set after = moved.execute("SELECT 1");
    CHECK(after.next());
  }

  // A pool the caller owns, built from the pieces: what it opens later comes
  // from the recipe it was handed, not from a string anyone spelled for it.
  {
    pool_options opts;
    opts.target = spec;
    opts.size = 1;
    opts.acquire_timeout = std::chrono::milliseconds(2000);
    opts.heartbeat_interval = std::chrono::milliseconds(0);
    connection_pool pool(opts);
    {
      orm from_pool(pool);
      result_set rows = from_pool.execute("SELECT 1");
      CHECK(rows.next());
    }
    CHECK(pool.idle_count() == 1);
    pooled_connection back = pool.acquire();
    CHECK(bool(back));
  }

  // A target that is not there is refused by the driver rather than asked
  // about: neither route may prompt, so this throws instead of waiting for
  // input -- SQLDriverConnect runs without a prompt, and a logon to a service
  // the name server does not know comes back with ORA-12154.
  connection_spec absent{ the_target.scheme, "uniorm_no_such_dsn", user, pwd };
  CHECK_THROWS(connection{ absent }, backend::backend_error);
}

bool same_row_except_name(User const& lhs, User const& rhs) {
  return lhs.id == rhs.id && lhs.age == rhs.age && lhs.balance == rhs.balance
    && lhs.note == rhs.note && lhs.created == rhs.created;
}

bool same_row(User const& lhs, User const& rhs) {
  return same_row_except_name(lhs, rhs) && lhs.name == rhs.name;
}

std::vector<User> read_range(orm& db) {
  return db.query<User>()
    .where(ge(&User::id, std::int64_t{ 9000 }))
    .order_by(&User::id)
    .all();
}

// One body per operation, each driven from a coroutine: the five terminals the
// synchronous face has for this fixture, reached through the session instead
// of through the calling thread.
async::task<void> drive_session(async::orm& session, std::size_t& inserted,
  std::size_t& updated, std::vector<User>& all_rows, std::optional<User>& one,
  std::vector<User>& by_sql) {
  std::vector<User> rows{
    User{ 9003, "gina", std::int32_t{ 44 }, 3.5, std::nullopt, std::nullopt },
    User{ 9004, "hank", std::nullopt, 4.5, std::string("async"),
      detail::make_timestamp(2026, 1, 2, 3, 4, 5, 0) }
  };
  inserted = co_await session.insert(rows);
  all_rows = co_await session.query<User>()
    .where(ge(&User::id, std::int64_t{ 9000 }))
    .order_by(&User::id)
    .all();
  one = co_await session.query<User>()
    .where(eq(&User::id, std::int64_t{ 9004 }))
    .one();
  updated = co_await session.execute_update(
    "UPDATE uniorm_it_user SET name = ? WHERE id = ?",
    params{ std::string("renamed"), std::int64_t{ 9004 } });
  by_sql = co_await session.query<User>(
    "SELECT id, name, age, balance, note, created FROM uniorm_it_user"
    " WHERE id >= ? ORDER BY id",
    params{ std::int64_t{ 9000 } });
  co_return;
}

// The asynchronous segment reads the fixture the synchronous path wrote, and
// every check compares one async read against the sync read of the same rows:
// a passing run says the two faces are one database path, not two look-alikes.
void test_async_reads(std::string const& conn_string, orm& db) {
  std::vector<User> seeded{
    User{ 9000, "amy", std::int32_t{ 20 }, 1.0, std::nullopt, std::nullopt },
    User{ 9001, "ben", std::nullopt, 2.0, std::string("sync"), std::nullopt },
    User{ 9002, "cid", std::int32_t{ 30 }, -1.0, std::nullopt, std::nullopt }
  };
  CHECK(db.insert(seeded) == 3);

  std::size_t inserted = 0;
  std::size_t updated = 0;
  std::vector<User> all_rows;
  std::optional<User> one;
  std::vector<User> by_sql;
  std::vector<User> by_callback;
  std::exception_ptr callback_failure;
  {
    async::orm session(conn_string);
    map_entities(session);
    auto work =
      drive_session(session, inserted, updated, all_rows, one, by_sql);
    work.block();
    CHECK(session.pending() == 0);

    // The callback face over the same session and the same statement the
    // coroutine face awaited. Nothing pumps it here: the answer is the
    // callback, so reading it back relies on the destructor waiting for it.
    session.query<User>(
      "SELECT id, name, age, balance, note, created FROM uniorm_it_user"
      " WHERE id >= ? ORDER BY id",
      params{ std::int64_t{ 9000 } },
      [&](std::vector<User> rows, std::exception_ptr bad) {
        callback_failure = bad;
        by_callback = std::move(rows);
      });
  }

  CHECK(!callback_failure);
  CHECK(inserted == 2);
  CHECK(updated == 1);
  CHECK(all_rows.size() == 5);
  CHECK(by_sql.size() == 5);
  CHECK(by_callback.size() == 5);

  auto const sync_rows = read_range(db);
  CHECK(sync_rows.size() == 5);
  for (std::size_t index = 0; index < sync_rows.size(); ++index) {
    // by_sql ran after this session's own rename, so it is what the
    // synchronous path now reads, field for field.
    CHECK(same_row(by_sql[index], sync_rows[index]));
    // The callback read the same five rows, after the same rename.
    CHECK(same_row(by_callback[index], sync_rows[index]));
    // all() ran before that rename: the same five rows, one name short.
    CHECK(same_row_except_name(all_rows[index], sync_rows[index]));
  }
  CHECK(all_rows[4].name == "hank");
  CHECK(sync_rows[4].name == "renamed");

  // The awaited one() is the row as insert() wrote it, NULLs and all.
  CHECK(one.has_value());
  CHECK(one->name == "hank");
  CHECK(!one->age.has_value());
  CHECK(one->note.value_or("") == "async");
  CHECK(one->created.has_value());

  auto const renamed = db.query<User>()
    .where(eq(&User::id, std::int64_t{ 9004 }))
    .one();
  CHECK(renamed.has_value());
  CHECK(renamed->name == "renamed");

  CHECK(db.execute_update("DELETE FROM uniorm_it_user WHERE id >= ?",
    params{ std::int64_t{ 9000 } }) == 5);
  CHECK(read_range(db).empty());
}

// A statement every driver this fixture runs on fails to prepare: a table
// nobody created. The synchronous call is the baseline for the text the driver
// produced, so a passing run says each asynchronous exit carries that report
// rather than a wrapper around it.
char const* k_missing_sql = "SELECT id, name FROM uniorm_it_async_missing";
char const* k_range_sql =
  "SELECT id, name, age, balance, note, created FROM uniorm_it_user"
  " WHERE id BETWEEN ? AND ? ORDER BY id";

std::string report_of(backend::backend_error const& error) {
  return std::string(error.what());
}

std::string report_of(std::exception_ptr bad) {
  if (!bad) {
    return std::string();
  }
  try {
    std::rethrow_exception(bad);
  } catch (backend::backend_error const& error) {
    return report_of(error);
  } catch (...) {
    return std::string();
  }
}

std::vector<User> read_band(orm& db, std::int64_t floor) {
  return db.query<User>()
    .where(ge(&User::id, floor))
    .order_by(&User::id)
    .all();
}

std::optional<User> row_at(orm& db, std::int64_t id) {
  return db.query<User>().where(eq(&User::id, id)).one();
}

// Every segment below works inside 9100..9199 and clears it on both ends, so a
// run that died in one of them does not decide what the next one counts.
void clear_band(orm& db) {
  db.execute_update(
    "DELETE FROM uniorm_it_user WHERE id BETWEEN 9100 AND 9199");
}

// The failure caught on the far side of a co_await, inside a body.
async::task<void> guard_one_read(async::orm& session, std::string& report,
  bool& reported, std::size_t& rows_read) {
  try {
    auto found = co_await session.query<User>(k_missing_sql);
    rows_read = found.size();
  } catch (backend::backend_error const& error) {
    reported = true;
    report = report_of(error);
  }
  co_return;
}

// The same failure with nothing catching it: it comes out of the drive action.
async::task<std::size_t> read_straight(async::orm& session) {
  auto found = co_await session.query<User>(k_missing_sql);
  co_return found.size();
}

void test_async_failure_reaches_every_exit(
  std::string const& conn_string, orm& db) {
  clear_band(db);

  std::string baseline;
  bool sync_reported = false;
  try {
    db.query<User>(k_missing_sql);
  } catch (backend::backend_error const& error) {
    sync_reported = true;
    CHECK(error.backend_name() == the_target.scheme);
    baseline = report_of(error);
  }
  CHECK(sync_reported);
  CHECK(baseline.find('[') != std::string::npos);

  std::string blocked_report;
  std::string awaited_report;
  std::string escaped_report;
  std::string handed_report;
  std::string callback_report;
  std::vector<User> callback_rows(1);
  std::size_t awaited_rows = 0;
  std::size_t escaped_rows = 0;
  bool blocked_reported = false;
  bool awaited_reported = false;
  bool guarded_escaped = false;
  bool escaped_reported = false;
  bool handed_reported = false;
  bool kept_reported = false;
  bool callback_answered = false;
  {
    async::orm session(conn_string);
    map_entities(session);

    try {
      session.query<User>(k_missing_sql).block();
    } catch (backend::backend_error const& error) {
      blocked_reported = true;
      blocked_report = report_of(error);
    }
    CHECK(session.pending() == 0);

    auto guarded =
      guard_one_read(session, awaited_report, awaited_reported, awaited_rows);
    try {
      guarded.block();
    } catch (...) {
      guarded_escaped = true;
    }

    try {
      auto straight = read_straight(session);
      escaped_rows = straight.block();
    } catch (backend::backend_error const& error) {
      escaped_reported = true;
      escaped_report = report_of(error);
    }
    CHECK(session.pending() == 0);

    session.query<User>(k_missing_sql)
      .start([&](std::exception_ptr bad) {
        handed_reported = true;
        handed_report = report_of(bad);
      });

    // The other half of what start() promises: a body that lands is reported
    // nowhere, and no thread here waits for this write.
    session.execute_update(
      "INSERT INTO uniorm_it_user (id, name, age, balance)"
      " VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 9100 }, std::string("handed"), std::int32_t{ 5 },
        1.0 })
      .start([&](std::exception_ptr) { kept_reported = true; });

    session.query<User>(k_missing_sql, params{},
      [&](std::vector<User> rows, std::exception_ptr bad) {
        callback_answered = true;
        callback_rows = std::move(rows);
        callback_report = report_of(bad);
      });
  }

  CHECK(blocked_reported);
  CHECK(awaited_reported);
  CHECK(!guarded_escaped);
  CHECK(escaped_reported);
  CHECK(handed_reported);
  CHECK(callback_answered);
  CHECK(!kept_reported);
  CHECK(awaited_rows == 0);
  CHECK(escaped_rows == 0);
  CHECK(blocked_report == baseline);
  CHECK(awaited_report == baseline);
  CHECK(escaped_report == baseline);
  CHECK(handed_report == baseline);
  CHECK(callback_report == baseline);
  CHECK(callback_rows.empty());

  auto const kept = row_at(db, std::int64_t{ 9100 });
  CHECK(kept.has_value());
  CHECK(kept->name == "handed");
  clear_band(db);
}

async::task<void> commit_a_write(async::orm& session, bool& opened,
  bool& closed) {
  auto tx = co_await session.begin();
  opened = tx.active();
  co_await session.execute_update(
    "INSERT INTO uniorm_it_user (id, name, age, balance)"
    " VALUES (?, ?, ?, ?)",
    params{ std::int64_t{ 9120 }, std::string("committed"), std::int32_t{ 6 },
      1.0 });
  co_await tx.commit();
  closed = !tx.active();
  co_return;
}

async::task<void> roll_back_a_write(async::orm& session, bool& closed) {
  auto tx = co_await session.begin();
  co_await session.execute_update(
    "INSERT INTO uniorm_it_user (id, name, age, balance)"
    " VALUES (?, ?, ?, ?)",
    params{ std::int64_t{ 9121 }, std::string("rolled"), std::int32_t{ 7 },
      1.0 });
  co_await tx.rollback();
  closed = !tx.active();
  co_return;
}

// Reaching the end of the body without either verb is a legal shape: the handle
// goes out of scope here, which is where this round trip happens.
async::task<void> forget_to_close(async::orm& session) {
  auto tx = co_await session.begin();
  co_await session.execute_update(
    "INSERT INTO uniorm_it_user (id, name, age, balance)"
    " VALUES (?, ?, ?, ?)",
    params{ std::int64_t{ 9122 }, std::string("forgotten"),
      std::int32_t{ 8 }, 1.0 });
  co_return;
}

void test_async_transaction_closes_its_round_trip(
  std::string const& conn_string, orm& db) {
  clear_band(db);
  bool opened = false;
  bool committed_closed = false;
  bool rolled_back_closed = false;
  {
    async::orm session(conn_string);
    map_entities(session);
    auto kept = commit_a_write(session, opened, committed_closed);
    kept.block();
    CHECK(opened);
    CHECK(committed_closed);

    auto dropped = roll_back_a_write(session, rolled_back_closed);
    dropped.block();
    CHECK(rolled_back_closed);

    auto forgotten = forget_to_close(session);
    forgotten.block();
    CHECK(session.pending() == 0);
  }

  CHECK(row_at(db, std::int64_t{ 9120 }).has_value());
  CHECK(!row_at(db, std::int64_t{ 9121 }).has_value());
  CHECK(!row_at(db, std::int64_t{ 9122 }).has_value());
  CHECK(read_band(db, std::int64_t{ 9120 }).size() == 1);
  clear_band(db);
}

// The four callback names against a real driver: each answer arrives as the
// second argument, and what the body did is what the synchronous face reads
// back once the session has gone quiet.
void test_async_callback_crud(std::string const& conn_string, orm& db) {
  clear_band(db);
  std::vector<User> seeded{
    User{ 9130, "nia", std::int32_t{ 31 }, 2.0, std::nullopt, std::nullopt },
    User{ 9131, "omar", std::int32_t{ 32 }, 3.0, std::string("cb"),
      std::nullopt }
  };

  std::size_t inserted = 0;
  std::size_t updated = 0;
  std::size_t removed = 0;
  std::size_t duplicate_count = 0;
  std::vector<User> read_back;
  std::vector<User> read_after;
  std::exception_ptr insert_failure;
  std::exception_ptr read_failure;
  std::exception_ptr update_failure;
  std::exception_ptr remove_failure;
  std::exception_ptr duplicate_failure;
  std::exception_ptr after_failure;
  {
    async::orm session(conn_string);
    map_entities(session);
    session.insert(seeded, [&](std::size_t added, std::exception_ptr bad) {
      inserted = added;
      insert_failure = bad;
    });
    session.query<User>(k_range_sql, params{ std::int64_t{ 9130 },
      std::int64_t{ 9131 } },
      [&](std::vector<User> rows, std::exception_ptr bad) {
        read_back = std::move(rows);
        read_failure = bad;
      });

    std::vector<User> changed{ seeded };
    changed[0].name = "nia_renamed";
    changed[1].balance = 9.5;
    session.update(changed, [&](std::size_t touched, std::exception_ptr bad) {
      updated = touched;
      update_failure = bad;
    });

    // The same keys again: the driver refuses, the answer is R{} with a real
    // error in the second argument, and the queue keeps serving after it.
    session.insert(seeded, [&](std::size_t added, std::exception_ptr bad) {
      duplicate_count = added;
      duplicate_failure = bad;
    });
    session.query<User>(k_range_sql, params{ std::int64_t{ 9130 },
      std::int64_t{ 9131 } },
      [&](std::vector<User> rows, std::exception_ptr bad) {
        read_after = std::move(rows);
        after_failure = bad;
      });

    std::vector<User> dropping{ seeded[1] };
    session.remove(dropping, [&](std::size_t gone, std::exception_ptr bad) {
      removed = gone;
      remove_failure = bad;
    });

    // The barrier ~orm() would run, taken while the session stays open: every
    // request this one accepted has answered by the time it comes back.
    session.wait_all_operations();
    CHECK(session.pending() == 0);
    CHECK(inserted == 2);
    CHECK(updated == 2);
    CHECK(removed == 1);
    CHECK(read_back.size() == 2);
    CHECK(read_after.size() == 2);
  }

  CHECK(!insert_failure);
  CHECK(!read_failure);
  CHECK(!update_failure);
  CHECK(!remove_failure);
  CHECK(!after_failure);
  CHECK(duplicate_failure);
  CHECK(inserted == 2);
  CHECK(updated == 2);
  CHECK(removed == 1);
  CHECK(duplicate_count == 0);
  CHECK(read_back.size() == 2);
  CHECK(same_row(read_back[0], seeded[0]));
  CHECK(same_row(read_back[1], seeded[1]));
  CHECK(read_after.size() == 2);
  CHECK(read_after[0].name == "nia_renamed");
  CHECK(read_after[1].balance == 9.5);

  auto const sync_rows = read_band(db, std::int64_t{ 9130 });
  CHECK(sync_rows.size() == 1);
  CHECK(sync_rows[0].id == 9130);
  CHECK(sync_rows[0].name == "nia_renamed");
  clear_band(db);
}

async::task<void> write_and_read_once(async::orm& session,
  std::size_t& written, std::vector<User>& rows) {
  std::vector<User> batch{
    User{ 9160, "opener", std::int32_t{ 60 }, 1.0, std::nullopt, std::nullopt }
  };
  written = co_await session.insert(batch);
  rows = co_await session.query<User>()
    .where(ge(&User::id, std::int64_t{ 9160 }))
    .all();
  co_return;
}

// A session that starts from nothing: the mapping goes through the forwarded
// map(), and the connection itself is one operation on the same queue as the
// work after it.
void test_async_session_connects_as_an_operation(
  std::string const& conn_string, orm& db) {
  clear_band(db);
  std::size_t written = 0;
  std::vector<User> rows;
  {
    async::orm session;
    session.map<User>(k_table)
      .primary_key("id", &User::id)
      .column("name", &User::name)
      .column("age", &User::age)
      .column("balance", &User::balance)
      .column("note", &User::note)
      .column("created", &User::created);
    session.connect(conn_string).block();
    auto work = write_and_read_once(session, written, rows);
    work.block();
    CHECK(session.pending() == 0);
    session.disconnect().block();
  }

  CHECK(written == 1);
  CHECK(rows.size() == 1);
  CHECK(rows[0].name == "opener");
  auto const stored = row_at(db, std::int64_t{ 9160 });
  CHECK(stored.has_value());
  CHECK(same_row(*stored, rows[0]));
  clear_band(db);
}

// The session takes the connection string itself: no synchronous object appears
// anywhere on this path, and the mapping written through the forwarded map() is
// the one the queued statement renders against a live driver.
void test_a_session_builds_on_its_own_connection_string(
  std::string const& conn_string, orm& db) {
  clear_band(db);
  std::size_t written = 0;
  std::vector<User> rows;
  {
    async::orm session(conn_string);
    session.map<User>(k_table)
      .primary_key("id", &User::id)
      .column("name", &User::name)
      .column("age", &User::age)
      .column("balance", &User::balance)
      .column("note", &User::note)
      .column("created", &User::created);
    std::vector<User> const batch{
      User{ 9170, "solo", std::int32_t{ 70 }, 1.0, std::nullopt, std::nullopt },
      User{ 9171, "standalone", std::int32_t{ 71 }, 2.0, std::string("direct"),
        std::nullopt }
    };
    written = session.insert(batch).block();
    rows = session.query<User>(
      k_range_sql, params{ std::int64_t{ 9170 }, std::int64_t{ 9171 } })
             .block();
    CHECK(session.auto_commit());
    CHECK(session.pending() == 0);
  }

  CHECK(written == 2);
  CHECK(rows.size() == 2);
  auto const stored = read_band(db, std::int64_t{ 9170 });
  CHECK(stored.size() == 2);
  CHECK(same_row(rows[0], *row_at(db, std::int64_t{ 9170 })));
  CHECK(same_row(rows[1], *row_at(db, std::int64_t{ 9171 })));
  clear_band(db);
}

// Two sessions are two connections, which is the only way this API goes in
// parallel: one thread drives a coroutine while the calling thread takes the
// callback answers. Each read is limited to the ids its own session wrote, so
// nothing here depends on how the two interleaved.
async::task<void> write_threaded_half(async::orm& session,
  std::size_t& written, std::int64_t& counted) {
  std::vector<User> batch{
    User{ 9140, "side-a", std::int32_t{ 40 }, 1.0, std::nullopt, std::nullopt },
    User{ 9141, "side-b", std::int32_t{ 41 }, 2.0, std::string("thread"),
      std::nullopt }
  };
  written = co_await session.insert(batch);
  counted = co_await session.query<User>()
    .where(ge(&User::id, std::int64_t{ 9140 }))
    .where(le(&User::id, std::int64_t{ 9141 }))
    .count();
  co_return;
}

void test_async_two_sessions_write_side_by_side(
  std::string const& conn_string, orm& db) {
  clear_band(db);
  std::size_t thread_written = 0;
  std::int64_t thread_counted = 0;
  std::exception_ptr thread_failure;
  std::size_t callback_written = 0;
  std::size_t callback_rows = 0;
  std::exception_ptr callback_failure;
  std::exception_ptr callback_read_failure;
  bool callback_read_answered = false;
  {
    std::thread other([&] {
      async::orm threaded(conn_string);
      map_entities(threaded);
      try {
        auto work = write_threaded_half(threaded, thread_written,
          thread_counted);
        work.block();
      } catch (...) {
        thread_failure = std::current_exception();
      }
    });

    async::orm local(conn_string);
    map_entities(local);
    std::vector<User> rows{
      User{ 9150, "side-c", std::int32_t{ 50 }, 1.0, std::nullopt,
        std::nullopt },
      User{ 9151, "side-d", std::int32_t{ 51 }, 2.0, std::string("two"),
        std::nullopt }
    };
    local.insert(rows, [&](std::size_t added, std::exception_ptr bad) {
      callback_written = added;
      callback_failure = bad;
    });
    local.query<User>(k_range_sql, params{ std::int64_t{ 9150 },
      std::int64_t{ 9151 } },
      [&](std::vector<User> found, std::exception_ptr bad) {
        callback_rows = found.size();
        callback_read_answered = true;
        callback_read_failure = bad;
      });

    other.join();
  }

  CHECK(!thread_failure);
  CHECK(!callback_failure);
  CHECK(!callback_read_failure);
  CHECK(thread_written == 2);
  CHECK(thread_counted == 2);
  CHECK(callback_written == 2);
  CHECK(callback_read_answered);
  CHECK(callback_rows == 2);

  auto const all = read_band(db, std::int64_t{ 9140 });
  CHECK(all.size() == 4);
  clear_band(db);
}

// One stage per call, so a statement this server refuses hides nothing that
// comes after it. A throw still counts against the run: an aborted stage is a
// failed stage unless the stage itself said why it gave up.
void guarded(char const* name, std::function<void()> body) {
  int const before = uniorm::test::failure_count();
  skip_reason = nullptr;
  try {
    body();
  } catch (std::exception const& e) {
    std::printf("ABORT %-50s %s\n", name,
      std::string(e.what()).substr(0, 90).c_str());
    ++uniorm::test::failure_count();
    return;
  }
  std::string verdict = skip_reason != nullptr
    ? std::string("skipped: ") + skip_reason
    : (uniorm::test::failure_count() == before ? "pass" : "FAIL");
  std::printf("%-50s %s\n", name, verdict.c_str());
}

}  // namespace

int main() {
  char const* dsn_env = std::getenv("UNIORM_IT_DSN");
  char const* user_env = std::getenv("UNIORM_IT_USER");
  char const* pwd_env = std::getenv("UNIORM_IT_PWD");
  if (!dsn_env || !*dsn_env || !user_env || !*user_env || !pwd_env ||
      !*pwd_env) {
    std::printf(
      "skip: set UNIORM_IT_DSN / UNIORM_IT_USER / UNIORM_IT_PWD to run "
      "integration tests\n");
    return 77;
  }
  char const* scheme_env = std::getenv("UNIORM_IT_BACKEND");
  std::string const scheme = (scheme_env && *scheme_env) ? scheme_env : "odbc";
  the_target.scheme = scheme;
  the_target.dsn = dsn_env;
  the_target.user = user_env;
  the_target.password = pwd_env;
  if (scheme == "odbc") {
    the_target.connect_string =
      "DSN=" + the_target.dsn + ";UID=" + the_target.user +
      ";PWD=" + the_target.password;
    the_target.integer_type = "BIGINT";
    the_target.wide_decimal = "DECIMAL(65,30)";
  } else if (scheme == "oracle") {
    the_target.connect_string = "oracle://" + the_target.user + "/" +
      the_target.password + "@" + the_target.dsn;
    // Two names this server does not answer with: it has no BIGINT, and its
    // DECIMAL stops at 38 digits of precision. Its own conversion also formats
    // a number by its digits rather than by the column's declared scale.
    the_target.integer_type = "NUMBER(19)";
    the_target.wide_decimal = "DECIMAL(38,30)";
    the_target.pads_decimal_text = false;
  } else if (scheme == "postgresql") {
    the_target.connect_string = "postgresql://" + the_target.user + ":" +
      the_target.password + "@" + the_target.dsn;
    // This server takes BIGINT for a 64-bit integral column, and an exact
    // number holds 38 digits of precision. Its own conversion of a numeric
    // pads the value to the column's declared scale.
    the_target.integer_type = "BIGINT";
    the_target.wide_decimal = "DECIMAL(38,30)";
    // This plugin prepares by a name of its own on the session it opened; a
    // driver that hides that work has no parse for this run to drop.
    the_target.holds_its_parses_by_name = true;
  } else {
    std::printf("skip: unknown UNIORM_IT_BACKEND '%s'\n", scheme.c_str());
    return 77;
  }

  try {
    connection probe(the_target.connect_string);
    std::printf("connected over '%s': dbms = %s\n", scheme.c_str(),
      probe.dbms_name().c_str());
    the_target.server_dialect = dialect::detect(probe.dbms_name());
    try {
      probe.schema();
      the_target.answers_catalog = true;
    } catch (backend::capability_not_supported const&) {
    }
  } catch (std::exception const& e) {
    std::printf("skip: cannot connect to '%s': %s\n", the_target.dsn.c_str(),
      e.what());
    return 77;
  }

  std::string const conn_string = the_target.connect_string;
  try {
    orm db = build_registry(conn_string);
    prepare_schema(db);
    seed_rows(db);

    guarded("test_dynamic_rows", [&] { test_dynamic_rows(db); });
    guarded("test_decimal_dynamic", [&] { test_decimal_dynamic(db); });
    guarded("test_number_text_ceiling",
      [&] { test_number_text_ceiling(db); });
    guarded("test_decimal_mapped", [&] { test_decimal_mapped(db); });
    guarded("test_zero_block_fetch_size",
      [&] { test_zero_block_fetch_size(db); });
    guarded("test_projection", [&] { test_projection(db); });
    guarded("test_registered_read_by_label",
      [&] { test_registered_read_by_label(db); });
    guarded("test_converter_round_trip",
      [&] { test_converter_round_trip(db); });
    guarded("test_validate", [&] { test_validate(conn_string); });
    guarded("test_identifier_spelling",
      [&] { test_identifier_spelling(conn_string); });
    guarded("test_identifier_case_twins",
      [&] { test_identifier_case_twins(conn_string); });

    guarded("test_query_builder", [&] { test_query_builder(db); });
    guarded("test_transaction", [&] { test_transaction(db); });
    guarded("test_auto_commit_scope",
      [&] { test_auto_commit_scope(db, conn_string); });
    guarded("test_insert", [&] { test_insert(db); });
    guarded("test_update", [&] { test_update(db); });
    guarded("test_remove", [&] { test_remove(db); });
    guarded("test_entity_update", [&] { test_entity_update(db); });
    guarded("test_composite_key_and_where_fields",
      [&] { test_composite_key_and_where_fields(db); });
    guarded("test_statement_cache", [&] { test_statement_cache(db); });
    guarded("test_a_dropped_parse_is_made_again",
      [&] { test_a_dropped_parse_is_made_again(db); });
    guarded("test_error_reporting", [&] { test_error_reporting(db); });
    guarded("test_pool", [&] { test_pool(conn_string); });
    guarded("test_pool_maintenance",
      [&] { test_pool_maintenance(conn_string); });
    guarded("test_connection_pieces",
      [&] { test_connection_pieces(the_target.dsn, the_target.user,
              the_target.password); });
    guarded("test_async_reads",
      [&] { test_async_reads(conn_string, db); });
    guarded("test_async_failure_reaches_every_exit",
      [&] { test_async_failure_reaches_every_exit(conn_string, db); });
    guarded("test_async_transaction_closes_its_round_trip",
      [&] { test_async_transaction_closes_its_round_trip(conn_string, db); });
    guarded("test_async_callback_crud",
      [&] { test_async_callback_crud(conn_string, db); });
    guarded("test_async_session_connects_as_an_operation",
      [&] { test_async_session_connects_as_an_operation(conn_string, db); });
    guarded("test_a_session_builds_on_its_own_connection_string",
      [&] { test_a_session_builds_on_its_own_connection_string(conn_string,
              db); });
    guarded("test_async_two_sessions_write_side_by_side",
      [&] { test_async_two_sessions_write_side_by_side(conn_string, db); });

    db.execute_update(std::string("DROP TABLE ") + k_table);
    db.execute_update(std::string("DROP TABLE ") + k_pair_table);
    db.execute_update(std::string("DROP TABLE ") + k_order_table);
    db.execute_update(std::string("DROP TABLE ") + k_dec_table);
    db.execute_update(std::string("DROP TABLE ") + k_money_table);
    db.execute_update(std::string("DROP TABLE ") + k_case_table);
  } catch (std::exception const& e) {
    std::printf("FATAL: unexpected exception: %s\n", e.what());
    ++uniorm::test::failure_count();
  }

  int failures = uniorm::test::failure_count();
  if (failures == 0) {
    std::printf("all integration tests passed\n");
    return 0;
  }
  std::printf("%d integration test(s) failed\n", failures);
  return 1;
}
