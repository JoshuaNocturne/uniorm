// Integration tests against a live database via ODBC DSN.
// DSN, user and password come from UNIORM_IT_DSN / UNIORM_IT_USER /
// UNIORM_IT_PWD; returns 77 (ctest SKIP) when any of them is unset or the
// database is unreachable. ASCII data only for now.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../unit/check.hpp"

#include <uniorm/backend/error.hpp>
#include <uniorm/connection.hpp>
#include <uniorm/converter.hpp>
#include <uniorm/decimal.hpp>
#include <uniorm/detail/identifier.hpp>
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
// Unquoted DDL, so each server folds it the way it folds any name: MySQL and
// MariaDB keep this spelling, PostgreSQL answers with all lower.
char const* k_case_table = "UNIORM_IT_CASE_USER";
std::string const long_note(1000, 'x');

void prepare_schema(orm& db) {
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_table);
  db.execute_update(std::string("CREATE TABLE ") + k_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " name VARCHAR(64) NOT NULL,"
                      " age INT NULL,"
                      " balance DOUBLE PRECISION NOT NULL,"
                      " note VARCHAR(2000) NULL,"
                      " created TIMESTAMP NULL)");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_pair_table);
  db.execute_update(std::string("CREATE TABLE ") + k_pair_table +
                      " (grp BIGINT NOT NULL,"
                      " idx BIGINT NOT NULL,"
                      " label VARCHAR(64) NOT NULL,"
                      " PRIMARY KEY (grp, idx))");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_order_table);
  db.execute_update(std::string("CREATE TABLE ") + k_order_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " state VARCHAR(16) NOT NULL,"
                      " note VARCHAR(16) NULL)");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_dec_table);
  db.execute_update(std::string("CREATE TABLE ") + k_dec_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " amount DECIMAL(20,4) NULL,"
                      " whole DECIMAL(20,0) NULL,"
                      " big DECIMAL(38,0) NULL)");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_money_table);
  db.execute_update(std::string("CREATE TABLE ") + k_money_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " amount DECIMAL(20,4) NOT NULL,"
                      " wide DECIMAL(65,30) NULL,"
                      " whole DECIMAL(20,0) NOT NULL)");
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_case_table);
  db.execute_update(std::string("CREATE TABLE ") + k_case_table +
                      " (ID BIGINT NOT NULL PRIMARY KEY,"
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
  CHECK(r.get<std::int64_t>("id") == 2);
  CHECK(r.get<std::int32_t>(0) == 2);  // by index, widened read
  CHECK(r.get<std::string>("name") == "bob");
  CHECK(r.is_null("age"));
  CHECK(r.get<std::string>("note").size() == long_note.size());
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
  CHECK(r1.get<std::string>("amount") == "12345678901234.5678");
  double amount = r1.get<double>("amount");
  CHECK(amount > 12345678901234.56 && amount < 12345678901234.57);
  CHECK_THROWS(r1.get<std::int64_t>("amount"), type_mismatch);  // fractional
  CHECK(r1.get<std::string>("whole") == "42");
  CHECK(r1.get<std::int64_t>("whole") == 42);
  CHECK(r1.get<std::string>("big") == "123456789012345678901234567890");
  CHECK_THROWS(r1.get<std::int64_t>("big"), type_mismatch);  // out of range

  CHECK(rs.next());
  row r2 = rs.current();
  CHECK(r2.get<std::string>("amount") == "0.1000");  // scale preserved

  CHECK(rs.next());
  row r3 = rs.current();
  CHECK(r3.is_null("amount"));
  CHECK_THROWS(r3.get<std::string>("amount"), type_mismatch);
  CHECK(!rs.next());
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

  auto back = db.query().of<Money>().order_by(&Money::id).all();
  CHECK(back.size() == 2);
  CHECK(back[0].amount.to_literal() == "12345678901234.5678");
  CHECK(back[0].whole.to_int64() == 42);
  // DECIMAL(65,30): no integer type holds this, and no double survives it.
  CHECK(back[0].wide.has_value() &&
        back[0].wide->to_literal() == "0.000000000000000000000000000001");
  CHECK_THROWS(back[0].wide->to_int64(), type_mismatch);
  CHECK(back[1].amount.to_literal() == "-0.0500");  // the column's scale
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
  CHECK(r.get<decimal_t>("amount") == decimal_t::from_literal("-0.05"));
  CHECK(r.get<std::string>("amount") == "-0.0500");

  // A decimal_t query value reaches the driver as that same literal.
  CHECK(db.query()
          .of<Money>()
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
    CHECK(!r.is_null("name"));
    if (!r.is_null("note")) {
      note_chars += r.get<std::string>("note").size();
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
    CHECK(r.get<std::string>("state") == "gold");
    CHECK(r.get<grade>("state") == grade::gold);
    CHECK(r.is_null("note"));
  }

  auto second = db.query()
                  .of<Order>()
                  .where(eq(&Order::id, std::int64_t{ 2 }))
                  .one();
  CHECK(second.has_value());
  CHECK(second->state == grade::silver);
  CHECK(second->note && *second->note == grade::bronze);

  auto listed = db.query().of<Order>().order_by(&Order::id).all();
  CHECK(listed.size() == 3);
  CHECK(listed[0].state == grade::gold);
  CHECK(!listed[0].note.has_value());  // NULL through the decoding stage
  CHECK(listed[2].note && *listed[2].note == grade::gold);

  auto in_list = db.query()
                   .of<Order>()
                   .where(in(&Order::state, { grade::bronze, grade::silver }))
                   .all();
  CHECK(in_list.size() == 2);

  auto rows = db.query<order_projection>(
    "SELECT id, state, note FROM uniorm_it_order ORDER BY id");
  CHECK(rows.size() == 3);
  CHECK(rows[1].state == grade::silver && rows[1].note == grade::bronze);
  CHECK(!rows[0].note.has_value());

  CHECK(db.query()
          .of<Order>()
          .where(eq(&Order::id, std::int64_t{ 3 }))
          .set(&Order::state, grade::silver)
          .set(&Order::note, nullptr)
          .update() == 1);
  auto edited = db.query()
                  .of<Order>()
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

  auto after = db.query().of<Order>().order_by(&Order::id).all();
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
  CHECK(db.query()
          .of<Order>()
          .where(eq(&Order::state, grade::gold))
          .count() == 1);

  db.execute_update(std::string("DELETE FROM ") + k_order_table);
}

orm build_registry(std::string_view conn_string) {
  orm db(conn_string);
  db.map<User>(k_table)
    .primary_key("id", &User::id)
    .column("name", &User::name)
    .column("age", &User::age)
    .column("balance", &User::balance)
    .column("note", &User::note)
    .column("created", &User::created);
  db.map<Pair>(k_pair_table)
    .primary_key("grp", &Pair::grp)
    .primary_key("idx", &Pair::idx)
    .column("label", &Pair::label);
  db.map<Order>(k_order_table)
    .primary_key("id", &Order::id)
    .column("state", &Order::state)
    .column("note", &Order::note);
  db.map<Money>(k_money_table)
    .primary_key("id", &Money::id)
    .column("amount", &Money::amount)
    .column("wide", &Money::wide)
    .column("whole", &Money::whole);
  return db;
}

void test_validate(std::string_view conn_string) {
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

// The policy's live half: folding to the spelling this server stores has to
// make both the check and the statement work, and missing it has to say so.
void test_identifier_spelling(std::string_view conn_string) {
  orm probe(conn_string);
  auto& md = probe.schema();
  std::string const lowered = detail::fold_lower(k_case_table);
  std::string stored;
  for (auto const& row : md.tables({}, {})) {
    if (detail::fold_lower(row.name) == lowered) {
      stored = row.name;
      break;
    }
  }
  CHECK(!stored.empty());
  if (stored.empty()) {
    return;
  }

  std::string const declared = stored == lowered ? k_case_table : lowered;
  // A server whose catalog read answers the declared spelling leaves no table
  // miss to name; the column check is exact, so the miss moves there.
  bool const table_apart = md.shape({ {}, {}, declared }).empty();

  // Declared opposite to the way this server reports a name, so whichever way
  // it folds, the mapping starts out disagreeing with the catalog.
  auto opposite = [](std::string const& name) {
    return name == detail::fold_lower(name) ? detail::fold_upper(name)
                                            : detail::fold_lower(name);
  };
  std::string reported_id;
  std::string id_column;
  std::string name_column;
  for (auto const& column : md.shape({ {}, {}, stored })) {
    std::string const folded = detail::fold_lower(column.name);
    if (folded == "id") {
      reported_id = column.name;
      id_column = opposite(column.name);
    } else if (folded == "name") {
      name_column = opposite(column.name);
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
  CHECK(table_fold == column_fold);
  if (!table_fold || table_fold != column_fold) {
    return;
  }

  orm db(conn_string);
  db.map<CaseUser>(declared)
    .primary_key(id_column, &CaseUser::id)
    .column(name_column, &CaseUser::name);

  // `keep` has to miss on every server, the question is only on which side.
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
  if (table_apart) {
    // The candidate carries the server's own schema qualification, so pin the
    // stored spelling rather than the whole message.
    CHECK(miss.find("table not found: " + declared) != std::string::npos);
    CHECK(miss.find("only case differs from") != std::string::npos);
    CHECK(miss.find(stored) != std::string::npos);
  } else {
    // The column registered first is the one the check reaches first.
    std::string const head =
      "column not found in table " + declared + ": " + id_column;
    CHECK(miss.find(head) != std::string::npos);
    CHECK(miss.find(reported_id) != std::string::npos);
  }

  db.identifier_case(*table_fold);
  db.validate();
  auto rows = db.query().of<CaseUser>().all();
  CHECK(rows.size() == 1);
  if (rows.size() == 1) {
    CHECK(rows[0].id == 1);
    CHECK(rows[0].name == "raised");
  }
}

void test_query_builder(orm& db) {
  CHECK(db.query().of<User>().count() == 3);

  auto all = db.query().of<User>().all();
  CHECK(all.size() == 3);

  auto one = db.query()
               .of<User>()
               .where(eq(&User::name, std::string("alice")))
               .one();
  CHECK(one.has_value() && one->id == 1 && one->age.value_or(0) == 30);
  CHECK(one->created.has_value());

  auto none = db.query()
                .of<User>()
                .where(eq(&User::name, std::string("nobody")))
                .one();
  CHECK(!none.has_value());

  auto adults = db.query().of<User>().where(gt(&User::age, 26)).all();
  CHECK(adults.size() == 1 && adults[0].id == 1);  // NULL age excluded

  auto nulls = db.query().of<User>().where(is_null(&User::age)).all();
  CHECK(nulls.size() == 1 && nulls[0].id == 2);

  auto picked =
    db.query()
      .of<User>()
      .where(in(&User::id, { std::int64_t{ 1 }, std::int64_t{ 3 } }))
      .all();
  CHECK(picked.size() == 2);

  auto liked =
    db.query().of<User>().where(like(&User::name, "a%")).all();
  CHECK(liked.size() == 1 && liked[0].name == "alice");

  auto combined =
    db.query()
      .of<User>()
      .where(gt(&User::age, 20) && ne(&User::name, std::string("carol")))
      .all();
  CHECK(combined.size() == 1 && combined[0].id == 1);

  auto page = db.query()
                .of<User>()
                .order_by(&User::id, direction::desc)
                .limit(2)
                .offset(1)
                .all();
  CHECK(page.size() == 2 && page[0].id == 2 && page[1].id == 1);

  std::string sql = db.query()
                      .of<User>()
                      .where(eq(&User::id, std::int64_t{ 1 }))
                      .build_select();
  // Which quote character lands here is the server's own doing: the dialect is
  // detected from the banner this connection read, not from the driver.
  dialect const d = dialect::detect(db.native_connection().dbms_name());
  CHECK(sql.find(d.quote_identifier("uniorm_it_user")) != std::string::npos);
  CHECK(sql.find(d.quote_identifier("id") + " = ?") != std::string::npos);

  // The spelling policy rides the same emission point, so one mapping can
  // serve a server that stores its names the other way.
  db.identifier_case(dialect::identifier_case::upper);
  std::string raised = db.query()
                         .of<User>()
                         .where(eq(&User::id, std::int64_t{ 1 }))
                         .build_select();
  CHECK(raised.find(d.quote_identifier("UNIORM_IT_USER")) != std::string::npos);
  CHECK(raised.find(d.quote_identifier("id") + " = ?") == std::string::npos);
  db.identifier_case(dialect::identifier_case::keep);
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
  CHECK(db.query().of<User>().count() == 3);

  {
    transaction tx = db.begin();
    db.execute_update("INSERT INTO uniorm_it_user (id, name, age, balance)"
                        " VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 101 }, std::string("kept"), nullptr, 0.0 });
    tx.commit();
  }
  CHECK(db.query().of<User>().count() == 4);

  {
    transaction tx = db.begin();  // destructor must roll back
    db.execute_update("INSERT INTO uniorm_it_user (id, name, age, balance)"
                        " VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 102 }, std::string("dropped"), nullptr, 0.0 });
  }
  CHECK(db.query().of<User>().count() == 4);

  db.execute_update(
    "DELETE FROM uniorm_it_user WHERE id = ?", params{ std::int64_t{ 101 } });
  CHECK(db.query().of<User>().count() == 3);
}

// auto_commit is the connection's commit mode, so it governs every write here:
// off, nothing is durable until commit(); on, each statement is.
void test_auto_commit_scope(orm& db, std::string const& conn_string) {
  // A run that died inside this section leaves 700 and up behind, and every
  // count below is over that range.
  db.execute_update("DELETE FROM uniorm_it_user WHERE id >= 700");
  CHECK(db.auto_commit());
  auto mine = [&db] {
    return db.query()
      .of<User>()
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
  CHECK(db.query().of<User>().count() == 6);

  auto erin = db.query()
                .of<User>()
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
  CHECK(db.query().of<User>().count() == 6 + 1500);

  db.execute_update(
    "DELETE FROM uniorm_it_user WHERE id >= ?", params{ std::int64_t{ 200 } });
  CHECK(db.query().of<User>().count() == 3);
}

void test_update(orm& db) {
  std::vector<User> users{
    User{ 400, "ivy", std::int32_t{ 33 }, 1.0, std::nullopt, std::nullopt },
    User{ 401, "jack", std::nullopt, 2.0, std::string("orig"), std::nullopt }
  };
  db.insert(users);
  CHECK(db.query().of<User>().count() == 5);

  // Dynamic update: bound values, including a NULL write.
  CHECK(db.update(k_table)
          .set("age", std::int32_t{ 34 })
          .set("note", nullptr)
          .where("id = ?", params{ std::int64_t{ 400 } })
          .execute() == 1);
  auto ivy = db.query()
               .of<User>()
               .where(eq(&User::id, std::int64_t{ 400 }))
               .one();
  CHECK(ivy.has_value() && ivy->age.value_or(0) == 34);
  CHECK(!ivy->note.has_value());

  // Entity update through the builder.
  CHECK(db.query()
          .of<User>()
          .where(eq(&User::id, std::int64_t{ 401 }))
          .set(&User::note, std::string("edited"))
          .set(&User::balance, 2.5)
          .update() == 1);
  auto jack = db.query()
                .of<User>()
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
  CHECK_THROWS(db.query()
                 .of<User>()
                 .where(eq(&User::id, std::int64_t{ 400 }))
                 .update(),
    uniorm_error);
  CHECK_THROWS(
    db.query().of<User>().set(&User::age, std::int32_t{ 1 }).update(),
    uniorm_error);

  db.execute_update(
    "DELETE FROM uniorm_it_user WHERE id >= ?", params{ std::int64_t{ 400 } });
  CHECK(db.query().of<User>().count() == 3);
}

void test_remove(orm& db) {
  std::vector<User> users{
    User{ 400, "ivy", std::int32_t{ 33 }, 1.0, std::nullopt, std::nullopt },
    User{ 401, "jack", std::nullopt, 2.0, std::nullopt, std::nullopt }
  };
  db.insert(users);
  CHECK(db.query().of<User>().count() == 5);

  // Entity delete through the builder.
  CHECK(db.query()
          .of<User>()
          .where(eq(&User::id, std::int64_t{ 400 }))
          .remove() == 1);
  CHECK(db.query().of<User>().count() == 4);

  // Dynamic delete.
  CHECK(db.remove(k_table)
          .where("id = ?", params{ std::int64_t{ 401 } })
          .execute() == 1);
  CHECK(db.query().of<User>().count() == 3);

  // Guards: blank where must throw.
  CHECK_THROWS(db.remove(k_table).execute(), uniorm_error);
  CHECK_THROWS(db.query().of<User>().remove(), uniorm_error);
}

void test_entity_update(orm& db) {
  // Insert a test row.
  User u{ 500, "eve", std::int32_t{ 28 }, 10.0, std::string("original"),
    std::nullopt };
  db.insert(std::vector<User>{ u });
  CHECK(db.query().of<User>().count() == 4);

  // Update using primary key as WHERE.
  User u2{ 500, "eve_updated", std::int32_t{ 29 }, 11.5, std::string("modified"),
    std::nullopt };
  CHECK(db.update(u2) == 1);
  auto row = db.query()
               .of<User>()
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
  auto row2 = db.query()
                .of<User>()
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
  CHECK(db.query().of<User>().count() == 3);
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
    return rs.current().get<std::string>("label");
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
      names.push_back(rs.current().get<std::string>("name"));
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
  CHECK(db.query().of<User>().all().size() == 3);
  CHECK(db.query().of<User>().all().size() == 3);
  CHECK(db.statement_cache_hits() == hits + 1);
  CHECK(db.statement_cache_size() > 0);
}

void test_error_reporting(orm& db) {
  // A driver failure surfaces as a backend_error: the ODBC layer throws
  // odbc_error, which derives from it, so callers never need the private
  // header to classify SQL errors or read their SQLSTATE.
  bool reported = false;
  try {
    db.execute("SELECT * FROM uniorm_it_no_such_table");
  } catch (backend::backend_error const& e) {
    reported = true;
    CHECK(e.backend_name() == "odbc");
    CHECK(!e.diagnostics().empty());
    if (!e.diagnostics().empty()) {
      auto const& first = e.diagnostics()[0];
      CHECK(first.state.size() == 5);
      CHECK(!first.message.empty());
      CHECK(std::string(e.what()).find("[" + first.state + "]") !=
            std::string::npos);
    }
  }
  CHECK(reported);
}

void test_pool(std::string const& conn_string) {
  pool_options opts;
  opts.connection_string = conn_string;
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
    opts.connection_string = conn_string;
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
    opts.connection_string = conn_string;
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
    opts.connection_string = conn_string;
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
    opts.connection_string = conn_string;
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
  std::string dsn = dsn_env;
  std::string user = user_env;
  std::string pwd = pwd_env;
  std::string conn_string = "DSN=" + dsn + ";UID=" + user + ";PWD=" + pwd;

  try {
    connection probe(conn_string);
    std::printf("connected: dbms = %s\n", probe.dbms_name().c_str());
  } catch (std::exception const& e) {
    std::printf(
      "skip: cannot connect to DSN '%s': %s\n", dsn.c_str(), e.what());
    return 77;
  }

  try {
    orm db = build_registry(conn_string);
    prepare_schema(db);
    seed_rows(db);

    test_dynamic_rows(db);
    test_decimal_dynamic(db);
    test_decimal_mapped(db);
    test_zero_block_fetch_size(db);
    test_projection(db);
    test_converter_round_trip(db);
    test_validate(conn_string);
    test_identifier_spelling(conn_string);

    test_query_builder(db);
    test_transaction(db);
    test_auto_commit_scope(db, conn_string);
    test_insert(db);
    test_update(db);
    test_remove(db);
    test_entity_update(db);
    test_composite_key_and_where_fields(db);
    test_statement_cache(db);
    test_error_reporting(db);
    test_pool(conn_string);
    test_pool_maintenance(conn_string);

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
