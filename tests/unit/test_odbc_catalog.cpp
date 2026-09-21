#include "odbc/schema_catalog.hpp"

#include "check.hpp"

using namespace uniorm;

namespace {

void test_exact_catalog_patterns() {
  using odbc::catalog_pattern_literal;
  using odbc::exact_catalog_arg_length;
  using unsupported = backend::capability_not_supported;

  CHECK(catalog_pattern_literal("ordinary", "\\") == "ordinary");
  CHECK(catalog_pattern_literal("", "\\").empty());
  CHECK(catalog_pattern_literal("a%_\\b", "\\") == "a\\%\\_\\\\b");
  CHECK(catalog_pattern_literal("a%_!b", "!") == "a!%!_!!b");
  CHECK(catalog_pattern_literal("%_", "%") == "%%%_");
  CHECK(catalog_pattern_literal("%_", "_") == "_%__");
  CHECK(catalog_pattern_literal("stra\xc3\x9f" "e", "\\") ==
    "stra\xc3\x9f" "e");
  CHECK_THROWS(catalog_pattern_literal("name", ""), unsupported);
  CHECK_THROWS(catalog_pattern_literal("name", "ab"), unsupported);
  CHECK_THROWS(catalog_pattern_literal("name", { "\0", 1 }), unsupported);
  CHECK_THROWS(catalog_pattern_literal({ "a\0b", 3 }, "\\"), unsupported);
  CHECK_THROWS(exact_catalog_arg_length({ "a\0b", 3 }), unsupported);

  auto limit = std::numeric_limits<SQLSMALLINT>::max();
  std::string boundary(static_cast<std::size_t>(limit), 'x');
  CHECK(exact_catalog_arg_length(boundary) == limit);
  CHECK(catalog_pattern_literal(boundary, "\\") == boundary);
  boundary.back() = '_';
  CHECK_THROWS(catalog_pattern_literal(boundary, "\\"), unsupported);
  boundary += 'x';
  CHECK_THROWS(exact_catalog_arg_length(boundary), unsupported);
  CHECK(exact_catalog_arg_length("") == 0);

  CHECK(odbc::exact_tables_catalog_arg("app_%", "\\") == "app_%");
  CHECK(odbc::exact_tables_catalog_arg("app\\db", "!") == "app\\db");
  CHECK_THROWS(odbc::exact_tables_catalog_arg("app\\db", "\\"), unsupported);
  CHECK_THROWS(odbc::exact_tables_catalog_arg("app!db", "!"), unsupported);
  CHECK_THROWS(odbc::exact_tables_catalog_arg("app", ""), unsupported);
  CHECK_THROWS(odbc::exact_tables_catalog_arg({ "a\0b", 3 }, "\\"),
    unsupported);
}

void test_exact_catalog_fields() {
  using odbc::exact_catalog_text;
  using odbc::odbc_error;

  char buffer[] = "name";
  std::string_view storage(buffer, sizeof(buffer));
  CHECK(exact_catalog_text(storage, 4, "table") == "name");
  for (auto field : { "catalog", "schema", "table", "column" }) {
    CHECK_THROWS(exact_catalog_text(storage, SQL_NULL_DATA, field),
      odbc_error);
    CHECK_THROWS(exact_catalog_text(storage, SQL_NO_TOTAL, field),
      odbc_error);
    CHECK_THROWS(exact_catalog_text(storage, 5, field), odbc_error);
    CHECK_THROWS(exact_catalog_text(storage, 100, field), odbc_error);
    CHECK_THROWS(exact_catalog_text(storage, -2, field), odbc_error);
    CHECK_THROWS(exact_catalog_text({ "\0", 1 }, 0, field), odbc_error);
  }
  CHECK(exact_catalog_text(storage, SQL_NULL_DATA, "schema", true).empty());
  CHECK(exact_catalog_text({ "\0", 1 }, 0, "schema", true).empty());
  CHECK_THROWS(exact_catalog_text(storage, 5, "schema", true), odbc_error);
  CHECK_THROWS(exact_catalog_text(storage, SQL_NO_TOTAL, "schema", true),
    odbc_error);
  CHECK_THROWS(exact_catalog_text({ "a\0b\0", 4 }, 3, "table"), odbc_error);
  CHECK_THROWS(exact_catalog_text({ "abcd", 4 }, 3, "table"), odbc_error);
  CHECK_THROWS(exact_catalog_text({}, 0, "table"), odbc_error);
}

void test_exact_catalog_identities() {
  using odbc::exact_catalog_namespace_matches;
  using odbc::exact_catalog_table_matches;

  schema_meta::table_ref asked{ "app_db", "app_%\\", "Users_%\\" };
  schema_meta::table_row row{ asked.catalog, asked.schema, asked.name };
  CHECK(exact_catalog_table_matches(asked, row));
  CHECK(exact_catalog_namespace_matches(asked.catalog, asked.schema, row));
  for (auto name : { "users_%\\", "Usersx%\\", "Users_abc\\", "" }) {
    row.name = name;
    CHECK(!exact_catalog_table_matches(asked, row));
    CHECK(exact_catalog_namespace_matches(asked.catalog, asked.schema, row));
  }
  row.name = asked.name;
  for (auto schema : { "APP_%\\", "appx%\\", "app_abc\\", "" }) {
    row.schema = schema;
    CHECK(!exact_catalog_table_matches(asked, row));
    CHECK(!exact_catalog_namespace_matches(asked.catalog, asked.schema, row));
  }
  row.schema = asked.schema;
  for (auto catalog : { "APP_DB", "appxdb", "", "another_db" }) {
    row.catalog = catalog;
    CHECK(!exact_catalog_table_matches(asked, row));
    CHECK(!exact_catalog_namespace_matches(asked.catalog, asked.schema, row));
  }
  row.catalog = asked.catalog;
  asked.schema.clear();
  CHECK(!exact_catalog_table_matches(asked, row));
  row.schema.clear();
  CHECK(exact_catalog_table_matches(asked, row));
  CHECK(exact_catalog_namespace_matches(asked.catalog, "", row));
  CHECK(!exact_catalog_namespace_matches("", "", row));
  asked.name = "stra\xc3\x9f" "e";
  row.name = "strasse";
  CHECK(!exact_catalog_table_matches(asked, row));
  row.name = asked.name;
  CHECK(exact_catalog_table_matches(asked, row));
}

struct legacy_catalog : schema_meta {
  bool legacy_read = false;

  std::string database_name() override { return {}; }
  std::vector<table_row> tables(std::string_view, std::string_view) override {
    legacy_read = true;
    return {};
  }
  std::vector<column_row> table_columns(table_ref const&) override {
    legacy_read = true;
    return {};
  }
  std::vector<std::string> primary_key(table_ref const&) override { return {}; }
  std::vector<foreign_key_row> foreign_keys(table_ref const&) override {
    return {};
  }
  std::vector<index_row> indexes(table_ref const&) override { return {}; }
};

void test_exact_catalog_defaults() {
  legacy_catalog catalog;
  CHECK_THROWS(catalog.exact_tables("app_db", "public"),
    backend::capability_not_supported);
  CHECK_THROWS(catalog.exact_table_columns({ "app_db", "public", "Users" }),
    backend::capability_not_supported);
  CHECK(!catalog.legacy_read);
}

}  // namespace

void test_odbc_catalog_name_matches() {
  // Asked for as a name: the driver's own spelling of the row decides.
  CHECK(odbc::catalog_name_matches("user_orders", "user_orders"));
  // Case is not a difference here; how a name is emitted is a policy, not a
  // fact about which rows a read was about.
  CHECK(odbc::catalog_name_matches("USER_ORDERS", "user_orders"));
  // A name widened by the pattern rules of a catalog call: a different table.
  CHECK(!odbc::catalog_name_matches("user_orders", "userxorders"));
  CHECK(!odbc::catalog_name_matches("a%", "abc"));
  CHECK(!odbc::catalog_name_matches("orders", "order"));
  CHECK(odbc::catalog_name_matches("a%", "a%"));
  // Nothing asked, or nothing reported by the driver, leaves the row alone.
  CHECK(odbc::catalog_name_matches("", "anything"));
  CHECK(odbc::catalog_name_matches("user_orders", ""));
  CHECK(odbc::catalog_name_matches("", ""));
  // Folding stops at ASCII, so a non-ASCII name cannot fold onto another:
  // both of these are 7 bytes.
  CHECK(!odbc::catalog_name_matches("strasse", "stra\xc3\x9f" "e"));
  CHECK(odbc::catalog_name_matches("stra\xc3\x9f" "e", "stra\xc3\x9f" "e"));

  test_exact_catalog_patterns();
  test_exact_catalog_fields();
  test_exact_catalog_identities();
  test_exact_catalog_defaults();
}
