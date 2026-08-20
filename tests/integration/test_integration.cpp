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

#include <uniorm/connection.hpp>
#include <uniorm/detail/time.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/pool.hpp>
#include <uniorm/query/builder.hpp>
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

}  // namespace uniorm

namespace {

char const* k_table = "uniorm_it_user";
std::string const long_note(1000, 'x');

void prepare_schema(orm& db) {
  db.execute_update(std::string("DROP TABLE IF EXISTS ") + k_table);
  db.execute_update(std::string("CREATE TABLE ") + k_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " name VARCHAR(64) NOT NULL,"
                      " age INT NULL,"
                      " balance DOUBLE NOT NULL,"
                      " note VARCHAR(2000) NULL,"
                      " created DATETIME NULL)");
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

orm build_registry(std::string_view conn_string) {
  orm db(conn_string);
  db.map<User>(k_table)
    .primary_key("id", &User::id)
    .column("name", &User::name)
    .column("age", &User::age)
    .column("balance", &User::balance)
    .column("note", &User::note)
    .column("created", &User::created);
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
  CHECK(sql.find("`uniorm_it_user`") != std::string::npos);  // MariaDB dialect
  CHECK(sql.find("`id` = ?") != std::string::npos);
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

  // Dynamic version without an entity mapping.
  std::size_t n = db.insert_batch(k_table,
    { "id", "name", "age", "balance", "note", "created" },
    { params{ std::int64_t{ 300 }, std::string("gina"), nullptr, 9.0, nullptr,
        nullptr },
      params{ std::int64_t{ 301 }, std::string("hank"), std::int32_t{ 22 }, 0.5,
        nullptr, nullptr } });
  CHECK(n == 2);
  CHECK(db.query().of<User>().count() == 8);

  CHECK(db.insert_batch(k_table, { "id", "name" }, {}) == 0);  // no rows

  // Every row must carry exactly one value per column.
  CHECK_THROWS(
    db.insert_batch(k_table, { "id", "name" },
      { params{ std::int64_t{ 302 }, std::string("x"), std::int32_t{ 1 } } }),
    uniorm_error);
  CHECK_THROWS(
    db.insert_batch(k_table, {}, { params{ std::int64_t{ 303 } } }),
    uniorm_error);
  CHECK(db.query().of<User>().count() == 8);  // nothing leaked

  // 1500 rows x 6 columns exceeds the per-statement placeholder cap,
  // forcing multiple multi-row VALUES statements in one transaction.
  std::vector<User> many;
  many.reserve(1500);
  for (std::int64_t id = 1000; id < 2500; ++id) {
    many.push_back(
      User{ id, "bulk", std::nullopt, 0.0, std::nullopt, std::nullopt });
  }
  CHECK(db.insert(many) == 1500);
  CHECK(db.query().of<User>().count() == 8 + 1500);

  db.execute_update(
    "DELETE FROM uniorm_it_user WHERE id >= ?", params{ std::int64_t{ 200 } });
  CHECK(db.query().of<User>().count() == 3);
}

void test_update(orm& db) {
  db.insert_batch(k_table, { "id", "name", "age", "balance", "note" },
    { params{ std::int64_t{ 400 }, std::string("ivy"), std::int32_t{ 33 }, 1.0,
        nullptr },
      params{ std::int64_t{ 401 }, std::string("jack"), nullptr, 2.0,
        std::string("orig") } });
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
  db.insert_batch(k_table, { "id", "name", "age", "balance" },
    { params{
        std::int64_t{ 400 }, std::string("ivy"), std::int32_t{ 33 }, 1.0 },
      params{ std::int64_t{ 401 }, std::string("jack"), nullptr, 2.0 } });
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
  db.insert_batch(k_table, { "id", "name", "age", "balance", "note" },
    { params{ std::int64_t{ 500 }, std::string("eve"), std::int32_t{ 28 }, 10.0,
      std::string("original") } });
  CHECK(db.query().of<User>().count() == 4);

  // Update using primary key as WHERE.
  User u{ 500, "eve_updated", std::int32_t{ 29 }, 11.5, std::string("modified"),
    std::nullopt };
  CHECK(db.update(u) == 1);
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
  User u2{ 501, "eve_updated", std::int32_t{ 30 }, 12.0, std::string("again"),
    std::nullopt };
  CHECK(db.update(u2, { "name" }) == 1);
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
    CHECK(a.get().is_open() && b.get().is_open());
    CHECK_THROWS(pool.acquire(), pool_timeout);  // exhausted
  }

  pooled_connection c = pool.acquire();  // returned by destructors above
  CHECK(bool(c));
  CHECK(c->is_open());
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
      CHECK(c.get().is_open());
    }
    CHECK(pool.idle_count() == 1);
    std::this_thread::sleep_for(300ms);
    CHECK(pool.heartbeats_executed() >= 1);  // maintainer ran the heartbeat
    CHECK(pool.idle_count() == 1);  // and kept the connection alive
    std::this_thread::sleep_for(2500ms);  // now idle beyond max_idle_time
    CHECK(pool.idle_count() == 0);

    pooled_connection again = pool.acquire();  // lazily recreated
    CHECK(bool(again));
    CHECK(again.get().is_open());
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
    CHECK(pool.idle_count() == 0);
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
    test_projection(db);
    test_validate(conn_string);

    test_query_builder(db);
    test_transaction(db);
    test_insert(db);
    test_update(db);
    test_remove(db);
    test_entity_update(db);
    test_statement_cache(db);
    test_pool(conn_string);
    test_pool_maintenance(conn_string);

    db.execute_update(std::string("DROP TABLE ") + k_table);
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
