#include "check.hpp"

#include <string>

#include <sqlext.h>

#include "config.hpp"
#include "generator.hpp"
#include "naming.hpp"
#include "schema_model.hpp"

using namespace uniorm::gen;

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
}

column_model make_column(std::string name, int native_type,
  std::string type_name, bool nullable, bool pk, std::int32_t size = 0,
  std::int16_t decimals = 0) {
  column_model c;
  c.name = std::move(name);
  c.data_type = native_type;
  c.type_name = std::move(type_name);
  c.nullable = nullable;
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
  order.columns.push_back(make_column("id", SQL_BIGINT, "BIGINT", false, true));
  order.columns.push_back(
    make_column("amount", SQL_NUMERIC, "NUMERIC", true, false, 10, 2));
  order.columns.push_back(
    make_column("status", SQL_VARCHAR, "VARCHAR", true, false, 32));
  order.columns.push_back(
    make_column("created", SQL_TYPE_TIMESTAMP, "TIMESTAMP", false, false));
  order.columns.push_back(
    make_column("payload", SQL_VARBINARY, "VARBINARY", true, false, 256));
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
  audit.columns.push_back(make_column("id", SQL_BIGINT, "BIGINT", false, true));
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

void test_generate_errors() {
  gen_config cfg;
  cfg.tables["t_order"].columns["status"].cpp_type = "money";
  CHECK_THROWS(generate_header(make_fixture(), cfg), config_error);

  gen_config conv;
  conv.tables["t_order"].columns["status"].converter = "status_converter";
  CHECK_THROWS(generate_header(make_fixture(), conv), config_error);
}

void test_generate_real_warning() {
  schema_model model;
  model.name = "db";
  table_model t;
  t.name = "t";
  t.columns.push_back(make_column("ratio", SQL_REAL, "FLOAT", false, false));
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
  test_generate_errors();
  test_generate_real_warning();
}
