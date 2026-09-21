#include "check.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <uniorm/backend/error.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/error.hpp>
#include <uniorm/builder/expression.hpp>
#include <uniorm/mapping/registry.hpp>

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
    // The policy that decides what a quoted name is spelled as. Keeping is
    // what quoting alone did before it existed.
    dialect keep;
    CHECK(keep.identifiers == dialect::identifier_case::keep);
    CHECK(keep.fold_identifier("User_Id") == "User_Id");
    CHECK(keep.quote_identifier("User_Id") == "\"User_Id\"");

    dialect lower;
    lower.identifiers = dialect::identifier_case::lower;
    CHECK(lower.fold_identifier("USER_ID") == "user_id");
    CHECK(lower.quote_identifier("USER_ID") == "\"user_id\"");
    // The server's own quote character survives the fold.
    dialect mysql = dialect::detect("MySQL");
    mysql.identifiers = dialect::identifier_case::upper;
    CHECK(mysql.quote_identifier("user_id") == "`USER_ID`");
    // Folding stops at ASCII, so a name in another alphabet is left alone.
    CHECK(lower.fold_identifier("Stra\xc3\x9f" "e") == "stra\xc3\x9f" "e");
  }
  {
    dialect mysql = dialect::detect("MySQL");
    CHECK(mysql.quote_open == '`' && mysql.quote_close == '`');
    CHECK(mysql.quote_identifier("col") == "`col`");
    CHECK(mysql.pagination(std::size_t{ 5 }, 10) == " LIMIT 5 OFFSET 10");
    CHECK(mysql.pagination(std::nullopt, 7) == " OFFSET 7");
    CHECK(mysql.table_qualification == dialect::qualification::catalog);
    CHECK(dialect::detect("PostgreSQL").ansi_pagination);
    CHECK(dialect::detect("pOsTgReSQL").table_qualification ==
          dialect::qualification::schema);
    CHECK(dialect::detect("mariadb").quote_open == '`');
    CHECK(dialect::detect("MariaDB").table_qualification ==
          dialect::qualification::catalog);
    CHECK(dialect{}.table_qualification ==
          dialect::qualification::unsupported);
    for (auto dbms_name : { "", "SQLite", "SQL Server", "Oracle" }) {
      CHECK(dialect::detect(dbms_name).table_qualification ==
            dialect::qualification::unsupported);
    }
  }
  {
    dialect postgres = dialect::detect("PostgreSQL");
    postgres.identifiers = dialect::identifier_case::lower;
    CHECK(postgres.quote_identifier("User\"Name") == "\"user\"\"name\"");
    CHECK(postgres.quote_exact_identifier("User\"Name") ==
          "\"User\"\"Name\"");
    CHECK(postgres.quote_exact_identifier("") == "\"\"");
    CHECK(postgres.quote_exact_identifier("App.Users") == "\"App.Users\"");
    CHECK(postgres.quote_exact_identifier("Stra\xc3\x9f" "e") ==
          "\"Stra\xc3\x9f" "e\"");

    dialect mysql = dialect::detect("MySQL");
    mysql.identifiers = dialect::identifier_case::upper;
    CHECK(mysql.quote_identifier("User`Name") == "`USER``NAME`");
    CHECK(mysql.quote_exact_identifier("User`Name") == "`User``Name`");

    dialect brackets;
    brackets.quote_open = '[';
    brackets.quote_close = ']';
    CHECK(brackets.quote_identifier("[User]Name]") == "[[User]]Name]]]");
    CHECK(brackets.quote_exact_identifier("[User]Name]") ==
          "[[User]]Name]]]");

    std::string_view nul_name("User\0Name", 9);
    CHECK_THROWS(postgres.quote_exact_identifier(nul_name), uniorm_error);
    CHECK_THROWS(postgres.quote_identifier(nul_name), uniorm_error);
    CHECK_THROWS(mysql.quote_exact_identifier(nul_name), uniorm_error);
    CHECK_THROWS(mysql.quote_identifier(nul_name), uniorm_error);
    CHECK_THROWS(brackets.quote_identifier(nul_name), uniorm_error);
  }
  {
    entity_meta meta;
    meta.table = "Declared.Users";
    mapping_builder<User> mapping(meta);
    mapping.primary_key("Declared_Id", &User::id)
      .column("Declared_Name", &User::name)
      .ignore(&User::email);
    CHECK(!meta.resolved);
    CHECK(meta.columns[0].is_primary_key);
    CHECK(!meta.columns[1].is_primary_key);
    CHECK(meta.ignored.size() == 1);

    dialect postgres = dialect::detect("PostgreSQL");
    postgres.identifiers = dialect::identifier_case::lower;
    auto id_key = make_member_key(&User::id);
    auto name_key = make_member_key(&User::name);
    auto email_key = make_member_key(&User::email);
    CHECK(meta.table_sql(postgres) == "\"declared.users\"");
    CHECK(meta.column_sql(0, postgres) == "\"declared_id\"");
    CHECK(meta.column_sql(name_key, postgres) == "\"declared_name\"");
    CHECK(meta.table_sql(dialect{}) == "\"Declared.Users\"");
    CHECK(meta.column_sql(id_key, dialect{}) == "\"Declared_Id\"");
    CHECK_THROWS(meta.column_sql(email_key, postgres), mapping_error);

    auto declared_labels = std::make_shared<column_names>(
      std::vector<std::string>{ "Declared_Name", "Declared_Id" });
    row declared_row(declared_labels, { std::string("Before"),
                                       std::int32_t{ 7 } });
    User user{};
    meta.populate(&user, declared_row);
    CHECK(user.id == 7);
    CHECK(user.name == "Before");

    meta.resolved = identifier_resolution{
      { "IgnoredCatalog", "App.Schema\"V1", "User\"Rows" },
      { "User_Id", "Display\"Name" }
    };
    CHECK(meta.table_sql(postgres) ==
          "\"App.Schema\"\"V1\".\"User\"\"Rows\"");
    CHECK(meta.column_sql(0, postgres) == "\"User_Id\"");
    CHECK(meta.column_sql(id_key, postgres) == "\"User_Id\"");
    CHECK(meta.column_sql(1, postgres) == "\"Display\"\"Name\"");
    CHECK(meta.column_sql(name_key, postgres) == "\"Display\"\"Name\"");
    CHECK(meta.table == "Declared.Users");
    CHECK(meta.columns[0].column == "Declared_Id");
    CHECK(meta.columns[1].column == "Declared_Name");
    CHECK(meta.column_name(id_key) == "Declared_Id");
    CHECK(meta.column_name(name_key) == "Declared_Name");
    CHECK_THROWS(meta.column_name(email_key), mapping_error);
    CHECK_THROWS(meta.column_sql(email_key, postgres), mapping_error);
    CHECK_THROWS(meta.table_sql(dialect{}),
      backend::capability_not_supported);
    CHECK_THROWS(meta.column_sql(0, dialect{}),
      backend::capability_not_supported);
    CHECK_THROWS(meta.column_sql(name_key, dialect{}),
      backend::capability_not_supported);

    auto resolved_labels = std::make_shared<column_names>(
      std::vector<std::string>{ "Display\"Name", "User_Id" });
    row resolved_row(resolved_labels, { std::string("After"),
                                       std::int32_t{ 8 } });
    meta.populate(&user, resolved_row);
    CHECK(user.id == 8);
    CHECK(user.name == "After");
    CHECK_THROWS(meta.populate(&user, declared_row), column_not_found);

    CHECK_THROWS(mapping.column("Extra", &User::email), mapping_error);
    CHECK_THROWS(mapping.primary_key("Extra", &User::email), mapping_error);
    CHECK_THROWS(mapping.ignore(&User::name), mapping_error);
    CHECK_THROWS(mapping_builder<User>(meta).column("Extra", &User::email),
      mapping_error);
    CHECK(meta.columns.size() == 2);
    CHECK(meta.ignored.size() == 1);
    CHECK(meta.ignored[0] == email_key);
    CHECK(meta.resolved->columns.size() == 2);
    CHECK(meta.table_sql(postgres) ==
          "\"App.Schema\"\"V1\".\"User\"\"Rows\"");
  }
  {
    entity_meta meta;
    meta.table = "declared_users";
    mapping_builder<User>(meta).column("declared_name", &User::name);
    meta.resolved = identifier_resolution{
      { "App.Database`V1", "IgnoredSchema", "User`Rows" },
      { "Display`Name" }
    };
    dialect mysql = dialect::detect("MySQL");
    mysql.identifiers = dialect::identifier_case::upper;
    CHECK(meta.table_sql(mysql) == "`App.Database``V1`.`User``Rows`");
    CHECK(meta.column_sql(0, mysql) == "`Display``Name`");
    CHECK(meta.column_sql(make_member_key(&User::name), mysql) ==
          "`Display``Name`");
    CHECK(meta.table_sql(dialect::detect("MariaDB")) ==
          "`App.Database``V1`.`User``Rows`");
  }
}
