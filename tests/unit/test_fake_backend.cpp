#include "check.hpp"
#include "fake_backend.hpp"

#include <optional>
#include <string>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/error.hpp>
#include <uniorm/connection.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/query/builder.hpp>
#include <uniorm/transaction.hpp>

// Every test below runs against the in-memory fake backend. The test
// target links no ODBC at all, so compiling this file proves the public
// core API never needs ODBC headers (design doc 5.1).

namespace {

using uniorm::connection;
using uniorm::orm;
using uniorm::params;
using uniorm::test::fake_connection;
using uniorm::test::fake_statement;

fake_connection& fake_of(connection& conn) {
  auto* fake = conn.native_handle<fake_connection>();
  CHECK(fake != nullptr);
  return *fake;
}

fake_statement::scripted_result users_result() {
  fake_statement::scripted_result r;
  r.columns.push_back({
    { "id", uniorm::sql_type::bigint, 0, false },
    { std::int64_t{ 1 }, std::int64_t{ 2 } },
  });
  r.columns.push_back({
    { "name", uniorm::sql_type::varchar, 64, false },
    { std::string{ "alice" }, std::string{ "bob" } },
  });
  r.columns.push_back({
    { "nick", uniorm::sql_type::varchar, 64, true },
    { std::string{ "al" }, std::monostate{} },
  });
  return r;
}

void test_connection_basics() {
  connection conn("fake://ignored");
  CHECK(conn.is_open());

  auto& fake = fake_of(conn);
  CHECK(fake.opened_with_ == "ignored");
  CHECK(conn.dbms_name() == "FakeDB");

  // The fake backend offers no typed extensions.
  CHECK(conn.extension<uniorm::backend::schema_metadata>() == nullptr);

  CHECK_THROWS(connection("nosuch://x"), uniorm::backend::unknown_scheme);
}

void test_execute_result_set() {
  connection conn("fake://");
  auto& fake = fake_of(conn);
  fake.script_next(users_result());

  auto rs = conn.execute("SELECT id, name, nick FROM users WHERE id > ?",
    params(std::int32_t{ 10 }));

  CHECK(rs.column_count() == 3);
  CHECK(rs.column(0).name == "id");
  CHECK(rs.column(0).type == uniorm::sql_type::bigint);
  CHECK(rs.column(2).nullable);

  CHECK(rs.next());
  auto r1 = rs.current();
  CHECK(r1.get<std::int64_t>(0) == 1);
  CHECK(r1.get<std::string>("name") == "alice");
  CHECK(r1.get<std::string>(2) == "al");
  CHECK(!r1.is_null("nick"));

  CHECK(rs.next());
  auto r2 = rs.current();
  CHECK(r2.get<std::int64_t>("id") == 2);
  CHECK(r2.is_null(2));

  CHECK(!rs.next());

  // Parameters reached the backend statement.
  CHECK(fake.last_statement_->bound_params().size() == 1);
  CHECK(std::get<std::int32_t>(fake.last_statement_->bound_params()[0]) == 10);
}

void test_execute_update() {
  connection conn("fake://");
  auto& fake = fake_of(conn);

  fake_statement::scripted_result r;
  r.affected = 3;
  fake.script_next(std::move(r));

  auto n = conn.execute_update("UPDATE users SET name = ? WHERE id = ?",
    params(std::string{ "x" }, std::int64_t{ 1 }));
  CHECK(n == 3);
}

struct user_row {
  std::int64_t id{};
  std::string name;
  std::optional<std::int32_t> age;
};

void test_projection_query() {
  connection conn("fake://");
  auto& fake = fake_of(conn);

  fake_statement::scripted_result r;
  r.columns.push_back({
    { "id", uniorm::sql_type::bigint, 0, false },
    { std::int64_t{ 1 }, std::int64_t{ 2 } },
  });
  r.columns.push_back({
    { "name", uniorm::sql_type::varchar, 64, false },
    { std::string{ "alice" }, std::string{ "bob" } },
  });
  r.columns.push_back({
    { "age", uniorm::sql_type::integer, 0, true },
    { std::int64_t{ 30 }, std::monostate{} },
  });
  fake.script_next(std::move(r));

  auto rows = conn.query<user_row>("SELECT id, name, age FROM users");
  CHECK(rows.size() == 2);
  CHECK(rows[0].id == 1);
  CHECK(rows[0].name == "alice");
  CHECK(rows[0].age.has_value() && *rows[0].age == 30);
  CHECK(rows[1].id == 2);
  CHECK(rows[1].name == "bob");
  CHECK(!rows[1].age.has_value());
}

struct person {
  std::int64_t id{};
  std::string name;
  std::optional<std::int32_t> age;
};

fake_statement::scripted_result person_result(std::vector<std::int64_t> ids,
  std::vector<std::string> names,
  std::vector<std::optional<std::int64_t>> ages) {
  fake_statement::scripted_result r;
  std::vector<uniorm::sql_value> id_values;
  std::vector<uniorm::sql_value> name_values;
  std::vector<uniorm::sql_value> age_values;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    id_values.push_back(ids[i]);
    name_values.push_back(names[i]);
    age_values.push_back(ages[i].has_value()
                           ? uniorm::sql_value{ *ages[i] }
                           : uniorm::sql_value{ std::monostate{} });
  }
  r.columns.push_back(
    { { "id", uniorm::sql_type::bigint, 0, false }, std::move(id_values) });
  r.columns.push_back({ { "name", uniorm::sql_type::varchar, 64, false },
    std::move(name_values) });
  r.columns.push_back(
    { { "age", uniorm::sql_type::integer, 0, true }, std::move(age_values) });
  return r;
}

void test_builder_query() {
  connection conn("fake://");
  auto& fake = fake_of(conn);

  orm reg;
  reg.map<person>("person")
    .primary_key("id", &person::id)
    .column("name", &person::name)
    .column("age", &person::age);

  // all(): two scripted rows, including a NULL in the optional column.
  fake.script_next(person_result(
    { 1, 2 }, { "alice", "bob" }, { std::int64_t{ 30 }, std::nullopt }));
  auto all = conn.query(reg).of<person>().all();
  CHECK(all.size() == 2);
  CHECK(all[0].id == 1);
  CHECK(all[0].name == "alice");
  CHECK(all[0].age.has_value() && *all[0].age == 30);
  CHECK(all[1].name == "bob");
  CHECK(!all[1].age.has_value());

  // one(): single row.
  fake.script_next(person_result({ 7 }, { "carol" }, { std::int64_t{ 22 } }));
  auto one = conn.query(reg).of<person>().one();
  CHECK(one.has_value());
  CHECK(one->id == 7);
  CHECK(one->name == "carol");

  // one() with no matching row.
  fake.script_next(person_result({}, {}, {}));
  auto none = conn.query(reg).of<person>().one();
  CHECK(!none.has_value());

  // count(): scripted COUNT(*) result.
  fake_statement::scripted_result cnt;
  cnt.columns.push_back({
    { "count", uniorm::sql_type::bigint, 0, false },
    { std::int64_t{ 42 } },
  });
  fake.script_next(std::move(cnt));
  CHECK(conn.query(reg).of<person>().count() == 42);
}

void test_insert_batch() {
  connection conn("fake://");
  auto& fake = fake_of(conn);

  fake_statement::scripted_result r;
  r.affected = 2;
  fake.script_next(std::move(r));

  auto inserted = conn.insert_batch("t", { "a", "b" },
    { params(std::int32_t{ 1 }, std::string{ "x" }),
      params(std::int32_t{ 2 }, std::string{ "y" }) });
  CHECK(inserted == 2);

  // One multi-row VALUES statement inside one committed transaction.
  auto const& sql = fake.last_statement_->prepared_sql();
  CHECK(sql.find("VALUES (?, ?), (?, ?)") != std::string::npos);
  CHECK(fake.commits_ == 1);
  CHECK(fake.rollbacks_ == 0);
  CHECK(fake.autocommit_log_.size() >= 2);
  CHECK(fake.autocommit_log_.front() == false);
  CHECK(fake.autocommit_log_.back() == true);
}

void test_statement_cache() {
  connection conn("fake://");
  auto& fake = fake_of(conn);

  auto misses_before = conn.statement_cache_misses();
  auto hits_before = conn.statement_cache_hits();
  auto created_before = fake.counters_.statements_created;

  for (int i = 0; i < 3; ++i) {
    fake_statement::scripted_result r;
    r.affected = 1;
    fake.script_next(std::move(r));
    conn.execute_update("UPDATE t SET a = 1");
  }

  // First execution prepares the statement; the next two reuse it.
  CHECK(conn.statement_cache_misses() == misses_before + 1);
  CHECK(conn.statement_cache_hits() == hits_before + 2);
  CHECK(fake.counters_.statements_created == created_before + 1);

  // Reuse resets the statement before each execution.
  CHECK(fake.counters_.reset_calls >= 2);
}

void test_transactions() {
  connection conn("fake://");
  auto& fake = fake_of(conn);

  // Explicit commit.
  {
    auto txn = conn.begin();
    CHECK(txn.active());
    txn.commit();
    CHECK(!txn.active());
  }
  CHECK(fake.commits_ == 1);
  CHECK(fake.rollbacks_ == 0);

  // Explicit rollback.
  {
    auto txn = conn.begin();
    txn.rollback();
  }
  CHECK(fake.rollbacks_ == 1);

  // Uncommitted work rolls back on destruction.
  {
    auto txn = conn.begin();
  }
  CHECK(fake.rollbacks_ == 2);

  // Every transaction leaves autocommit restored.
  CHECK(fake.autocommit_log_.back() == true);
}

void test_validate_requires_schema_metadata() {
  connection conn("fake://");
  orm reg;
  reg.map<person>("person").primary_key("id", &person::id);

  CHECK_THROWS(reg.validate(conn), uniorm::mapping_error);
}

}  // namespace

void test_fake_backend() {
  test_connection_basics();
  test_execute_result_set();
  test_execute_update();
  test_projection_query();
  test_builder_query();
  test_insert_batch();
  test_statement_cache();
  test_transactions();
  test_validate_requires_schema_metadata();
}
