#include "check.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <uniorm/dialect.hpp>
#include <uniorm/error.hpp>
#include <uniorm/query/expression.hpp>

using namespace uniorm;

namespace uniorm {

struct User {
  std::int32_t id;
  std::string name;
  std::optional<std::string> email;
};

}  // namespace uniorm

namespace {

std::string resolve_key(member_key const& key) {
  if (key == make_member_key(&User::id))
    return "\"id\"";
  if (key == make_member_key(&User::name))
    return "\"name\"";
  return "\"email\"";
}

}  // namespace

void test_expression() {
  predicate::resolver resolve = resolve_key;

  {
    std::vector<sql_value> bound;
    CHECK(eq(&User::id, 7).to_sql(resolve, bound) == "\"id\" = ?");
    CHECK(bound.size() == 1);
    CHECK(std::get<std::int32_t>(bound[0]) == 7);
  }
  {
    std::vector<sql_value> bound;
    CHECK(ne(&User::name, std::string("x")).to_sql(resolve, bound) ==
          "\"name\" <> ?");
    CHECK(std::get<std::string>(bound[0]) == "x");
  }
  {
    std::vector<sql_value> bound;
    CHECK(lt(&User::id, 3).to_sql(resolve, bound) == "\"id\" < ?");
    CHECK(le(&User::id, 3).to_sql(resolve, bound) == "\"id\" <= ?");
    CHECK(gt(&User::id, 3).to_sql(resolve, bound) == "\"id\" > ?");
    CHECK(ge(&User::id, 3).to_sql(resolve, bound) == "\"id\" >= ?");
    CHECK(bound.size() == 4);
  }
  {
    std::vector<sql_value> bound;
    CHECK((col(&User::id) == 1).to_sql(resolve, bound) == "\"id\" = ?");
    CHECK(bound.size() == 1);
  }
  {
    std::vector<sql_value> bound;
    auto p = eq(&User::id, 1) &&
             (eq(&User::name, std::string("a")) || is_null(&User::email));
    std::string sql = p.to_sql(resolve, bound);
    CHECK(sql == "(\"id\" = ? AND (\"name\" = ? OR \"email\" IS NULL))");
    CHECK(bound.size() == 2);
  }
  {
    std::vector<sql_value> bound;
    CHECK(in(&User::id, { 1, 2, 3 }).to_sql(resolve, bound) ==
          "\"id\" IN (?, ?, ?)");
    CHECK(bound.size() == 3);
    bound.clear();
    CHECK(in(&User::id, std::vector<int>{}).to_sql(resolve, bound) == "1 = 0");
    CHECK(bound.empty());
  }
  {
    std::vector<sql_value> bound;
    CHECK(is_not_null(&User::email).to_sql(resolve, bound) ==
          "\"email\" IS NOT NULL");
    CHECK(bound.empty());
  }
  {
    std::vector<sql_value> bound;
    CHECK(like(&User::name, "a%").to_sql(resolve, bound) == "\"name\" LIKE ?");
    CHECK(std::get<std::string>(bound[0]) == "a%");
  }
  {
    std::vector<sql_value> bound;
    CHECK_THROWS(predicate{}.to_sql(resolve, bound), uniorm_error);
  }

  {
    dialect d;  // ANSI defaults
    CHECK(d.quote_identifier("col") == "\"col\"");
    CHECK(d.pagination(std::size_t{ 5 }, 10) ==
          " OFFSET 10 ROWS FETCH NEXT 5 ROWS ONLY");
    CHECK(d.pagination(std::size_t{ 5 }, 0) ==
          " OFFSET 0 ROWS FETCH NEXT 5 ROWS ONLY");
    CHECK(d.pagination(std::nullopt, 0).empty());
  }
  {
    dialect m = dialect::detect("MySQL");
    CHECK(m.quote_open == '`' && m.quote_close == '`');
    CHECK(m.quote_identifier("col") == "`col`");
    CHECK(m.pagination(std::size_t{ 5 }, 10) == " LIMIT 5 OFFSET 10");
    CHECK(m.pagination(std::nullopt, 7) == " OFFSET 7");
    CHECK(dialect::detect("PostgreSQL").ansi_pagination);
    CHECK(dialect::detect("mariadb").quote_open == '`');
  }
}
