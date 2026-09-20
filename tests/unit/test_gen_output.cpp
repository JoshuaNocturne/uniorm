#include "check.hpp"

#include <string>

#include "config.hpp"
#include "generator.hpp"
#include "naming.hpp"
#include "schema_model.hpp"

using namespace uniorm::gen;
using uniorm::sql_type;

namespace {

void test_naming() {
  CHECK(to_pascal_case("t_user") == "TUser");
  CHECK(to_pascal_case("user_profile") == "UserProfile");
  CHECK(to_pascal_case("HTTPCode") == "HttpCode");
  CHECK(to_pascal_case("2fa_log") == "_2faLog");
  CHECK(to_pascal_case("") == "_");

  CHECK(to_camel_case("user_id") == "userId");
  CHECK(to_camel_case("id") == "id");
  CHECK(to_camel_case("DELETE") == "delete_");
  CHECK(to_camel_case("order-item") == "orderItem");

  CHECK(to_unit_name("My-DB.1") == "my_db_1");
  CHECK(to_unit_name("") == "_");

  CHECK(fold_lower("USER_ID") == "user_id");
  CHECK(fold_upper("numeric") == "NUMERIC");
}

column_model make_column(std::string name, sql_type type,
  std::string type_name, bool nullable, bool pk, std::int32_t size = 0,
  std::int16_t decimals = 0) {
  column_model c;
  c.shape.name = std::move(name);
  c.shape.type = type;
  c.type_name = std::move(type_name);
  c.shape.nullable = nullable;
  c.primary_key = pk;
  c.size = size;
  c.decimals = decimals;
  return c;
}

schema_model make_fixture() {
  schema_model model;
  model.name = "testdb";

  table_model order;
  order.name = "t_order";
  order.columns.push_back(
    make_column("id", sql_type::bigint, "BIGINT", false, true));
  order.columns.push_back(
    make_column("amount", sql_type::decimal, "NUMERIC", true, false, 10, 2));
  order.columns.push_back(
    make_column("status", sql_type::varchar, "VARCHAR", true, false, 32));
  order.columns.push_back(
    make_column("created", sql_type::timestamp, "TIMESTAMP", false, false));
  order.columns.push_back(make_column(
    "payload", sql_type::varbinary, "VARBINARY", true, false, 256));
  fk_model fk;
  fk.pk_table = "t_user";
  fk.columns.emplace_back("user_id", "id");
  order.foreign_keys.push_back(fk);
  index_model idx;
  idx.name = "idx_status";
  idx.columns.push_back("status");
  order.indexes.push_back(idx);
  model.tables.push_back(std::move(order));

  table_model audit;
  audit.name = "t_audit";
  audit.columns.push_back(
    make_column("id", sql_type::bigint, "BIGINT", false, true));
  model.tables.push_back(std::move(audit));
  return model;
}

bool contains(std::string const& haystack, std::string const& needle) {
  return haystack.find(needle) != std::string::npos;
}

void test_generate() {
  gen_config cfg;
  cfg.type_overrides["NUMERIC(10,2)"] = "std::int64_t";
  cfg.tables["t_audit"].skip = true;

  generated_output out = generate_header(make_fixture(), cfg);
  std::string const& text = out.text;

  CHECK(contains(text, "#pragma once"));
  CHECK(contains(text, "#include <uniorm/mapping/registry.hpp>"));
  CHECK(contains(text, "namespace testdb {"));
  CHECK(contains(text, "// FK: user_id -> t_user(id)"));
  CHECK(contains(text, "// index: idx_status (status)"));
  CHECK(contains(text, "struct TOrder {"));
  CHECK(contains(text, "std::int64_t id;  // BIGINT PK NOT NULL"));
  CHECK(
    contains(text, "std::optional<std::int64_t> amount;  // NUMERIC(10,2)"));
  CHECK(contains(text, "std::optional<std::string> status;  // VARCHAR(32)"));
  CHECK(contains(text, "uniorm::timestamp created;  // TIMESTAMP NOT NULL"));
  CHECK(contains(
    text, "std::optional<std::vector<std::byte>> payload;  // VARBINARY(256)"));
  CHECK(contains(text, ".primary_key(\"id\", &TOrder::id)"));
  CHECK(contains(text, ".column(\"amount\", &TOrder::amount)"));
  CHECK(contains(text, "inline void register_testdb_schema(uniorm::orm&"));
  CHECK(contains(text, "register_TOrder_mapping(registry);"));
  CHECK(!contains(text, "t_audit"));  // skipped
  CHECK(contains(text, "}  // namespace testdb"));
}

void test_generate_class_override() {
  gen_config cfg;
  cfg.tables["t_order"].class_name = "Order";
  generated_output out = generate_header(make_fixture(), cfg);
  CHECK(contains(out.text, "struct Order {"));
  CHECK(contains(out.text, ".primary_key(\"id\", &Order::id)"));
  CHECK(contains(out.text, "register_Order_mapping"));
}

void test_generate_converter() {
  gen_config cfg;
  cfg.tables["t_order"].columns["status"].converter = "order_status";
  generated_output out = generate_header(make_fixture(), cfg);
  std::string const& text = out.text;

  // The member is the domain type, not the VARCHAR it binds as, and the
  // header insists on the specialization that makes the mapping compile.
  CHECK(contains(text, "std::optional<order_status> status;"));
  CHECK(contains(text, ".column(\"status\", &TOrder::status)"));
  CHECK(contains(
    text, "static_assert(uniorm::has_converter<order_status>,"));
  CHECK(out.warnings.empty());
}

void test_generate_decimal_member() {
  gen_config cfg;
  cfg.tables["t_order"].columns["amount"].cpp_type = "uniorm::decimal_t";
  generated_output out = generate_header(make_fixture(), cfg);
  std::string const& text = out.text;

  // The type arrives with the library, so unlike a domain type it needs no
  // specialization the caller has to declare first.
  CHECK(contains(text, "std::optional<uniorm::decimal_t> amount;"));
  CHECK(!contains(text, "has_converter<uniorm::decimal_t>"));
  CHECK(out.warnings.empty());
}

void test_generate_errors() {
  gen_config cfg;
  cfg.tables["t_order"].columns["status"].cpp_type = "money";
  CHECK_THROWS(generate_header(make_fixture(), cfg), config_error);
}

// A config written against one catalog's spelling still applies to another's,
// and the names reaching the SQL strings are the catalog's, not the config's.
void test_generate_case_insensitive_config() {
  gen_config cfg = parse_config(
    "[tables.T_ORDER]\nclass = \"Order\"\n"
    "[tables.T_ORDER.columns.STATUS]\ncpp_type = \"std::int16_t\"\n");
  generated_output out = generate_header(make_fixture(), cfg);

  CHECK(contains(out.text, "struct Order {"));
  CHECK(contains(out.text, "std::optional<std::int16_t> status;"));
  CHECK(contains(out.text, ".column(\"status\", &Order::status)"));
}

void test_generate_uppercase_catalog() {
  schema_model model;
  model.name = "db";
  table_model table;
  table.name = "USER_ACCOUNTS";
  table.columns.push_back(
    make_column("USER_ID", sql_type::bigint, "BIGINT", false, true));
  model.tables.push_back(std::move(table));

  gen_config cfg =
    parse_config("[tables.user_accounts]\nclass = \"Account\"\n");
  generated_output out = generate_header(model, cfg);

  CHECK(contains(out.text, "struct Account {"));
  CHECK(contains(out.text, "registry.map<Account>(\"USER_ACCOUNTS\")"));
  CHECK(contains(out.text, ".primary_key(\"USER_ID\", &Account::userId)"));
}

// Two tables that arrive at one class name cannot both be declared, so the run
// stops rather than write a header that will not compile.
void test_generate_class_name_collision() {
  schema_model model;
  model.name = "db";
  for (char const* name : { "user_accounts", "user_accounts_" }) {
    table_model table;
    table.name = name;
    table.columns.push_back(
      make_column("USER_ID", sql_type::bigint, "BIGINT", false, true));
    model.tables.push_back(std::move(table));
  }

  CHECK_THROWS(generate_header(model, gen_config{}), config_error);
  try {
    generate_header(model, gen_config{});
  } catch (config_error const& e) {
    CHECK(std::string(e.what()) ==
      "tables collide on one class name: 'UserAccounts' names user_accounts, "
      "user_accounts_. Set class in one of their [tables.*] sections.");
  }

  gen_config cfg =
    parse_config("[tables.user_accounts]\nclass = \"Legacy\"\n");
  generated_output out = generate_header(model, cfg);
  CHECK(contains(out.text, "struct Legacy {"));
  CHECK(contains(out.text, "struct UserAccounts {"));
}

// Names differing by case alone are one config key, so a class override moves
// both tables with it: the report has to rule that advice out.
void test_generate_case_only_collision() {
  schema_model model;
  model.name = "db";
  for (char const* name : { "users", "USERS" }) {
    table_model table;
    table.name = name;
    table.columns.push_back(
      make_column("ID", sql_type::bigint, "BIGINT", false, true));
    model.tables.push_back(std::move(table));
  }

  CHECK_THROWS(generate_header(model, gen_config{}), config_error);
  try {
    generate_header(model, gen_config{});
  } catch (config_error const& e) {
    CHECK(std::string(e.what()) ==
      "tables collide on one class name: 'Users' names users, USERS. users, "
      "USERS differ by case alone, which no [tables.*] section can split: ask "
      "for one of them with --tables.");
  }

  gen_config cfg = parse_config("[tables.users]\nclass = \"Legacy\"\n");
  CHECK_THROWS(generate_header(model, cfg), config_error);
  try {
    generate_header(model, cfg);
  } catch (config_error const& e) {
    CHECK(contains(e.what(), "'Legacy' names users, USERS"));
  }
}

void test_check_config() {
  schema_model model = make_fixture();
  std::vector<std::string> catalog = { "T_ORDER", "t_audit", "t_report" };

  check_config(parse_config("[tables.T_ORDER.columns.STATUS]\n"
                            "cpp_type = \"std::int16_t\"\n"
                            "[tables.T_AUDIT]\nskip = true\n"),
    model, catalog);
  // A table this run left out reads no columns, so its block is not judged on
  // names the catalog listing cannot settle.
  check_config(
    parse_config("[tables.t_report.columns.anything]\ncpp_type = \"double\"\n"),
    model, catalog);

  CHECK_THROWS(
    check_config(parse_config("[tables.t_orders]\nskip = true\n"), model,
      catalog),
    config_error);
  CHECK_THROWS(
    check_config(parse_config("[tables.t_order.columns.status2]\n"
                              "cpp_type = \"double\"\n"),
      model, catalog),
    config_error);

  try {
    check_config(
      parse_config("[tables.t_orders]\nskip = true\n"), model, catalog);
  } catch (config_error const& e) {
    CHECK(std::string(e.what()).find("[tables.t_orders]") != std::string::npos);
  }
}

void test_generate_real_warning() {
  schema_model model;
  model.name = "db";
  table_model t;
  t.name = "t";
  t.columns.push_back(
    make_column("ratio", sql_type::real, "FLOAT", false, false));
  model.tables.push_back(std::move(t));
  generated_output out = generate_header(model, gen_config{});
  CHECK(contains(out.text, "double ratio;"));
  CHECK(out.warnings.size() == 1);
}

}  // namespace

void test_gen_output() {
  test_naming();
  test_generate();
  test_generate_class_override();
  test_generate_converter();
  test_generate_decimal_member();
  test_generate_errors();
  test_generate_real_warning();
  test_generate_case_insensitive_config();
  test_generate_uppercase_catalog();
  test_generate_class_name_collision();
  test_generate_case_only_collision();
  test_check_config();
}
