#include "check.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "schema_reader.hpp"
#include "uniorm/error.hpp"

using namespace uniorm::gen;

namespace {

// A catalog that answers the two readings the generator has to join, each free
// to spell the same column its own way.
class fake_meta : public uniorm::schema_meta {
public:
  struct table_def {
    std::string name;
    std::vector<std::string> columns;
    std::vector<std::string> key;
  };

  std::vector<table_def> defs;
  std::vector<table_ref> asked;

  std::string database_name() override { return "fake"; }

  std::vector<table_row> tables(
    std::string_view catalog, std::string_view schema) override {
    std::vector<table_row> rows;
    for (table_def const& d : defs) {
      rows.push_back(table_row{
        std::string(catalog), std::string(schema), d.name });
    }
    return rows;
  }

  std::vector<column_row> table_columns(table_ref const& ref) override {
    std::vector<column_row> rows;
    for (std::string const& name : find(ref).columns) {
      column_row row;
      row.shape.name = name;
      row.shape.type = uniorm::sql_type::varchar;
      row.type_name = "VARCHAR";
      rows.push_back(std::move(row));
    }
    return rows;
  }

  std::vector<std::string> primary_key(table_ref const& ref) override {
    return find(ref).key;
  }

  std::vector<foreign_key_row> foreign_keys(table_ref const& ref) override {
    find(ref);
    return {};
  }

  std::vector<index_row> indexes(table_ref const& ref) override {
    find(ref);
    return {};
  }

private:
  // A miss is a read narrowed by an identity the backend never listed; the
  // tests see it in `asked` instead of throwing out of a virtual.
  table_def const& find(table_ref const& ref) {
    asked.push_back(ref);
    static table_def const none;
    for (table_def const& d : defs) {
      if (d.name == ref.name) {
        return d;
      }
    }
    return none;
  }
};

bool mentions(std::vector<std::string> const& warnings,
  std::string const& needle) {
  for (std::string const& w : warnings) {
    if (w.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void test_primary_key_marks_the_column() {
  fake_meta md;
  md.defs.push_back({ "USERS", { "USER_ID", "TENANT_ID" }, { "USER_ID" } });
  std::vector<std::string> warnings;
  schema_model model = read_schema(md, read_options{}, &warnings);

  CHECK(warnings.empty());
  CHECK(model.tables.size() == 1);
  CHECK(model.tables[0].columns[0].primary_key);
  CHECK(!model.tables[0].columns[1].primary_key);
  CHECK(md.asked.size() == 4);
  CHECK(md.asked[0].name == "USERS");
}

void test_primary_key_differing_only_in_case() {
  fake_meta md;
  md.defs.push_back({ "USERS", { "USER_ID" }, { "user_id" } });
  std::vector<std::string> warnings;
  schema_model model = read_schema(md, read_options{}, &warnings);

  CHECK(!model.tables[0].columns[0].primary_key);
  CHECK(mentions(warnings,
    "primary key names no such column: user_id (only case differs from "
    "'USER_ID')"));
}

void test_primary_key_naming_no_column() {
  fake_meta md;
  md.defs.push_back({ "USERS", { "USER_ID" }, { "CREATED_BY" } });
  std::vector<std::string> warnings;
  read_schema(md, read_options{}, &warnings);

  CHECK(mentions(warnings, "primary key names no such column: CREATED_BY"));
  CHECK(!mentions(warnings, "only case differs"));
}

void test_selected_table_keeps_the_catalog_name() {
  fake_meta md;
  md.defs.push_back({ "USERS", { "USER_ID" }, { "USER_ID" } });
  md.defs.push_back({ "ORDERS", { "ID" }, { "ID" } });
  read_options opts;
  opts.tables = { "users" };
  std::vector<std::string> warnings;
  schema_model model = read_schema(md, opts, &warnings);

  // The request's spelling selects, the catalog's spelling is what generation
  // and the per-table reads go on.
  CHECK(model.tables.size() == 1);
  CHECK(model.tables[0].name == "USERS");
  CHECK(warnings.empty());
  CHECK(std::all_of(md.asked.begin(), md.asked.end(),
    [](auto const& ref) { return ref.name == "USERS"; }));
}

void test_missing_table_and_empty_table() {
  fake_meta md;
  md.defs.push_back({ "USERS", { "USER_ID" }, { "USER_ID" } });
  read_options opts;
  opts.tables = { "nope" };
  CHECK_THROWS(read_schema(md, opts), uniorm::uniorm_error);

  fake_meta empty;
  empty.defs.push_back({ "EMPTY", {}, {} });
  std::vector<std::string> warnings;
  read_schema(empty, read_options{}, &warnings);
  CHECK(warnings.size() == 1);
  CHECK(warnings[0] == "table has no columns: EMPTY");
}

// A wildcard in a narrowing argument used to be the driver's to expand. Asked
// as a name now, it matches nothing, so an empty run has to say why.
void test_pattern_argument_is_warned() {
  fake_meta md;
  md.defs.push_back({ "USERS", { "USER_ID" }, { "USER_ID" } });
  read_options opts;
  opts.schema = "SALES%";
  std::vector<std::string> warnings;
  read_schema(md, opts, &warnings);

  CHECK(mentions(warnings, "--schema='SALES%'"));
  CHECK(mentions(warnings, "is not a wildcard here"));
}

}  // namespace

void test_gen_reader() {
  test_primary_key_marks_the_column();
  test_primary_key_differing_only_in_case();
  test_primary_key_naming_no_column();
  test_selected_table_keeps_the_catalog_name();
  test_missing_table_and_empty_table();
  test_pattern_argument_is_warned();
}
