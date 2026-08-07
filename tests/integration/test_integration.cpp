// Integration tests against a live database via ODBC DSN.
// DSN comes from UNIORM_IT_DSN (default: docker_maria). Returns 77 (ctest
// SKIP) when the database is unreachable. ASCII data only for now.

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

void prepare_schema(connection& conn) {
  conn.execute_update(std::string("DROP TABLE IF EXISTS ") + k_table);
  conn.execute_update(std::string("CREATE TABLE ") + k_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " name VARCHAR(64) NOT NULL,"
                      " age INT NULL,"
                      " balance DOUBLE NOT NULL,"
                      " note VARCHAR(2000) NULL,"
                      " created DATETIME NULL)");
}

void seed_rows(connection& conn) {
  timestamp ts = detail::make_timestamp(2024, 1, 2, 3, 4, 5, 0);
  std::size_t n1 = conn.execute_update(
    "INSERT INTO uniorm_it_user (id, name, age, balance, note, created)"
    " VALUES (?, ?, ?, ?, ?, ?)",
    params{ std::int64_t{ 1 }, std::string("alice"), std::int32_t{ 30 }, 12.5,
      nullptr, ts });
  CHECK(n1 == 1);
  std::size_t n2 = conn.execute_update(
    "INSERT INTO uniorm_it_user (id, name, age, balance, note, created)"
    " VALUES (?, ?, ?, ?, ?, ?)",
    params{ std::int64_t{ 2 }, std::string("bob"), nullptr, 0.0, long_note,
      nullptr });
  CHECK(n2 == 1);
  std::size_t n3 = conn.execute_update(
    "INSERT INTO uniorm_it_user (id, name, age, balance, note, created)"
    " VALUES (?, ?, ?, ?, ?, ?)",
    params{ std::int64_t{ 3 }, std::string("carol"), std::int32_t{ 25 }, -4.25,
      std::string("short"), nullptr });
  CHECK(n3 == 1);
}

void test_dynamic_rows(connection& conn) {
  result_set rs =
    conn.execute("SELECT id, name, age, note FROM uniorm_it_user WHERE id = ?",
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

void test_projection(connection& conn) {
  struct user_row {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
    double balance;
    std::optional<std::string> note;
    std::optional<timestamp> created;
  };

  auto rows = conn.query<user_row>(
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

orm build_registry() {
  orm registry;
  registry.map<User>(k_table)
    .primary_key("id", &User::id)
    .column("name", &User::name)
    .column("age", &User::age)
    .column("balance", &User::balance)
    .column("note", &User::note)
    .column("created", &User::created);
  return registry;
}

void test_validate(connection& conn) {
  orm registry = build_registry();
  registry.validate(conn);  // strict: must pass
  registry.validate(conn, validation_mode::lenient);

  struct Bad {
    std::int64_t id = 0;
  };
  {
    orm bad;
    bad.map<Bad>(k_table).primary_key("no_such_col", &Bad::id);
    CHECK_THROWS(bad.validate(conn), mapping_error);
  }
  {
    orm bad;
    bad.map<Bad>("no_such_table").primary_key("id", &Bad::id);
    CHECK_THROWS(bad.validate(conn), mapping_error);
  }
  {
    struct NoOpt {
      std::int64_t id = 0;
      std::int32_t age = 0;  // non-optional against nullable column
    };
    orm strict_reg;
    strict_reg.map<NoOpt>(k_table)
      .primary_key("id", &NoOpt::id)
      .column("age", &NoOpt::age);
    CHECK_THROWS(strict_reg.validate(conn), mapping_error);
    try {
      strict_reg.validate(conn, validation_mode::lenient);
      CHECK(true);
    } catch (...) {
      CHECK(false);
    }
  }
}

void test_query_builder(connection& conn, orm& registry) {
  CHECK(conn.query(registry).of<User>().count() == 3);

  auto all = conn.query(registry).of<User>().all();
  CHECK(all.size() == 3);

  auto one = conn.query(registry)
               .of<User>()
               .where(eq(&User::name, std::string("alice")))
               .one();
  CHECK(one.has_value() && one->id == 1 && one->age.value_or(0) == 30);
  CHECK(one->created.has_value());

  auto none = conn.query(registry)
                .of<User>()
                .where(eq(&User::name, std::string("nobody")))
                .one();
  CHECK(!none.has_value());

  auto adults = conn.query(registry).of<User>().where(gt(&User::age, 26)).all();
  CHECK(adults.size() == 1 && adults[0].id == 1);  // NULL age excluded

  auto nulls = conn.query(registry).of<User>().where(is_null(&User::age)).all();
  CHECK(nulls.size() == 1 && nulls[0].id == 2);

  auto picked =
    conn.query(registry)
      .of<User>()
      .where(in(&User::id, { std::int64_t{ 1 }, std::int64_t{ 3 } }))
      .all();
  CHECK(picked.size() == 2);

  auto liked =
    conn.query(registry).of<User>().where(like(&User::name, "a%")).all();
  CHECK(liked.size() == 1 && liked[0].name == "alice");

  auto combined =
    conn.query(registry)
      .of<User>()
      .where(gt(&User::age, 20) && ne(&User::name, std::string("carol")))
      .all();
  CHECK(combined.size() == 1 && combined[0].id == 1);

  auto page = conn.query(registry)
                .of<User>()
                .order_by(&User::id, direction::desc)
                .limit(2)
                .offset(1)
                .all();
  CHECK(page.size() == 2 && page[0].id == 2 && page[1].id == 1);

  std::string sql = conn.query(registry)
                      .of<User>()
                      .where(eq(&User::id, std::int64_t{ 1 }))
                      .build_select();
  CHECK(sql.find("`uniorm_it_user`") != std::string::npos);  // MariaDB dialect
  CHECK(sql.find("`id` = ?") != std::string::npos);
}

void test_transaction(connection& conn, orm& registry) {
  {
    transaction tx = conn.begin();
    CHECK(tx.active());
    conn.execute_update("INSERT INTO uniorm_it_user (id, name, age, balance)"
                        " VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 100 }, std::string("temp"), nullptr, 0.0 });
    tx.rollback();
    CHECK(!tx.active());
  }
  CHECK(conn.query(registry).of<User>().count() == 3);

  {
    transaction tx = conn.begin();
    conn.execute_update("INSERT INTO uniorm_it_user (id, name, age, balance)"
                        " VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 101 }, std::string("kept"), nullptr, 0.0 });
    tx.commit();
  }
  CHECK(conn.query(registry).of<User>().count() == 4);

  {
    transaction tx = conn.begin();  // destructor must roll back
    conn.execute_update("INSERT INTO uniorm_it_user (id, name, age, balance)"
                        " VALUES (?, ?, ?, ?)",
      params{ std::int64_t{ 102 }, std::string("dropped"), nullptr, 0.0 });
  }
  CHECK(conn.query(registry).of<User>().count() == 4);

  conn.execute_update(
    "DELETE FROM uniorm_it_user WHERE id = ?", params{ std::int64_t{ 101 } });
  CHECK(conn.query(registry).of<User>().count() == 3);
}

void test_insert(connection& conn, orm& registry) {
  std::vector<User> users;
  users.push_back(User{ 200, "dave", std::int32_t{ 40 }, 1.5,
    std::string("batched"), std::nullopt });
  users.push_back(User{ 201, "erin", std::nullopt, 0.0, std::nullopt,
    detail::make_timestamp(2025, 6, 7, 8, 9, 10, 0) });
  users.push_back(
    User{ 202, "frank", std::int32_t{ 51 }, -2.0, std::nullopt, std::nullopt });

  CHECK(conn.insert(registry, users) == 3);
  CHECK(conn.query(registry).of<User>().count() == 6);

  auto erin = conn.query(registry)
                .of<User>()
                .where(eq(&User::id, std::int64_t{ 201 }))
                .one();
  CHECK(erin.has_value());
  CHECK(erin->name == "erin");
  CHECK(!erin->age.has_value());  // empty optional written as NULL
  CHECK(!erin->note.has_value());
  CHECK(erin->created.has_value());

  // Dynamic version without an entity mapping.
  std::size_t n = conn.insert_batch(k_table,
    { "id", "name", "age", "balance", "note", "created" },
    { params{ std::int64_t{ 300 }, std::string("gina"), nullptr, 9.0, nullptr,
        nullptr },
      params{ std::int64_t{ 301 }, std::string("hank"), std::int32_t{ 22 }, 0.5,
        nullptr, nullptr } });
  CHECK(n == 2);
  CHECK(conn.query(registry).of<User>().count() == 8);

  CHECK(conn.insert_batch(k_table, { "id", "name" }, {}) == 0);  // no rows

  // Every row must carry exactly one value per column.
  CHECK_THROWS(
    conn.insert_batch(k_table, { "id", "name" },
      { params{ std::int64_t{ 302 }, std::string("x"), std::int32_t{ 1 } } }),
    uniorm_error);
  CHECK_THROWS(
    conn.insert_batch(k_table, {}, { params{ std::int64_t{ 303 } } }),
    uniorm_error);
  CHECK(conn.query(registry).of<User>().count() == 8);  // nothing leaked

  // 1500 rows x 6 columns exceeds the per-statement placeholder cap,
  // forcing multiple multi-row VALUES statements in one transaction.
  std::vector<User> many;
  many.reserve(1500);
  for (std::int64_t id = 1000; id < 2500; ++id) {
    many.push_back(
      User{ id, "bulk", std::nullopt, 0.0, std::nullopt, std::nullopt });
  }
  CHECK(conn.insert(registry, many) == 1500);
  CHECK(conn.query(registry).of<User>().count() == 8 + 1500);

  conn.execute_update(
    "DELETE FROM uniorm_it_user WHERE id >= ?", params{ std::int64_t{ 200 } });
  CHECK(conn.query(registry).of<User>().count() == 3);
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
  std::string dsn = dsn_env && *dsn_env ? dsn_env : "docker_maria";
  std::string user = user_env && *user_env ? user_env : "Joshua";
  std::string pwd = pwd_env && *pwd_env ? pwd_env : "joshua";
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
    connection conn(conn_string);
    prepare_schema(conn);
    seed_rows(conn);

    test_dynamic_rows(conn);
    test_projection(conn);
    test_validate(conn);

    orm registry = build_registry();
    test_query_builder(conn, registry);
    test_transaction(conn, registry);
    test_insert(conn, registry);
    test_pool(conn_string);
    test_pool_maintenance(conn_string);

    conn.execute_update(std::string("DROP TABLE ") + k_table);
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
