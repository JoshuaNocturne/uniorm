#include "check.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "ddl_meta.hpp"
#include "generator.hpp"
#include "schema_model.hpp"
#include "schema_reader.hpp"
#include "uniorm/error.hpp"

using namespace uniorm::gen;
using uniorm::schema_meta;
using uniorm::sql_type;

using column_row = schema_meta::column_row;
using foreign_key_row = schema_meta::foreign_key_row;
using index_row = schema_meta::index_row;

namespace {

bool mentions(std::vector<std::string> const& warnings,
  std::string const& needle) {
  return std::any_of(warnings.begin(), warnings.end(),
    [&](std::string const& w) { return w.find(needle) != std::string::npos; });
}

ddl_table const* table_of(
  ddl_snapshot const& snapshot, std::string const& name) {
  for (ddl_table const& table : snapshot.tables) {
    if (table.ref.name == name) {
      return &table;
    }
  }
  return nullptr;
}

column_row const* column_of(
  ddl_table const& table, std::string const& name) {
  for (column_row const& column : table.columns) {
    if (column.shape.name == name) {
      return &column;
    }
  }
  return nullptr;
}

// One foreign key as the comment in a generated header spells it, so a reading
// that is out of range reports nothing instead of indexing past the end.
std::string fk_of(ddl_table const& table, std::size_t index) {
  if (index >= table.foreign_keys.size()) {
    return {};
  }
  foreign_key_row const& key = table.foreign_keys[index];
  return key.column + " -> " + key.referenced_table + "(" +
    key.referenced_column + ")";
}

bool index_is(ddl_table const& table, std::string const& name,
  std::vector<std::string> const& columns, bool unique) {
  for (index_row const& index : table.indexes) {
    if (index.name == name) {
      return index.columns == columns && index.unique == unique;
    }
  }
  return false;
}

// One column read out of a one-column table, so a type spelling is tested the
// way a script carries it.
column_row single_column(std::string const& definition) {
  ddl_snapshot const snapshot =
    parse_ddl("CREATE TABLE t (c " + definition + ");");
  CHECK(snapshot.tables.size() == 1);
  CHECK(snapshot.tables[0].columns.size() == 1);
  if (snapshot.tables.empty() || snapshot.tables[0].columns.empty()) {
    static column_row const none;
    return none;
  }
  return snapshot.tables[0].columns[0];
}

void test_number_and_boolean_types() {
  CHECK(single_column("BIGINT").shape.type == sql_type::bigint);
  CHECK(single_column("BIGINT UNSIGNED").shape.type == sql_type::bigint);
  CHECK(single_column("INT8").shape.type == sql_type::bigint);
  CHECK(single_column("BIGSERIAL").shape.type == sql_type::bigint);
  CHECK(single_column("INT").shape.type == sql_type::integer);
  CHECK(single_column("MEDIUMINT").shape.type == sql_type::integer);
  CHECK(single_column("SERIAL").shape.type == sql_type::integer);
  CHECK(single_column("SMALLINT").shape.type == sql_type::smallint);
  CHECK(single_column("TINYINT").shape.type == sql_type::smallint);
  CHECK(single_column("FLOAT").shape.type == sql_type::real);
  CHECK(single_column("REAL").shape.type == sql_type::real);
  CHECK(single_column("FLOAT8").shape.type == sql_type::double_precision);
  CHECK(single_column("BOOL").shape.type == sql_type::boolean);
  CHECK(single_column("BOOLEAN").shape.type == sql_type::boolean);
  // A serial column is a sequence's to fill, so no catalog calls it nullable.
  CHECK(!single_column("SERIAL").shape.nullable);
  CHECK(single_column("INTEGER").type_name == "INTEGER");
  CHECK(single_column("int").type_name == "INT");
}

void test_text_and_binary_types() {
  CHECK(single_column("CHAR(2)").shape.type == sql_type::character);
  CHECK(single_column("VARCHAR(64)").shape.type == sql_type::varchar);
  CHECK(single_column("CHARACTER VARYING(8)").type_name ==
    "CHARACTER VARYING");
  CHECK(single_column("CHARACTER VARYING(8)").shape.type == sql_type::varchar);
  CHECK(single_column("TEXT").shape.type == sql_type::longvarchar);
  CHECK(single_column("LONG VARCHAR").shape.type == sql_type::longvarchar);
  CHECK(single_column("NCHAR(4)").shape.type == sql_type::wchar);
  CHECK(single_column("NVARCHAR(4)").shape.type == sql_type::wvarchar);
  CHECK(single_column("BINARY(4)").shape.type == sql_type::binary);
  CHECK(single_column("VARBINARY(16)").shape.type == sql_type::varbinary);
  CHECK(single_column("BLOB").shape.type == sql_type::varbinary);
  CHECK(single_column("BYTEA").shape.type == sql_type::varbinary);
  CHECK(single_column("UUID").shape.type == sql_type::guid);
  CHECK(single_column("JSONB").shape.type == sql_type::longvarchar);
  CHECK(single_column("ENUM('a','b')").shape.type == sql_type::varchar);
  // A type outside the mapped set keeps its own spelling and answers other,
  // which is what a live read answers with: the generator then warns.
  column_row const unknown = single_column("GEOMETRY");
  CHECK(unknown.shape.type == sql_type::other);
  CHECK(unknown.type_name == "GEOMETRY");
}

void test_date_time_and_decimal_arguments() {
  CHECK(single_column("DATE").shape.type == sql_type::date);
  CHECK(single_column("TIME").shape.type == sql_type::time);
  CHECK(single_column("DATETIME").shape.type == sql_type::timestamp);
  CHECK(single_column("TIMESTAMPTZ").shape.type == sql_type::timestamp);
  CHECK(single_column("TIMESTAMP WITHOUT TIME ZONE").type_name ==
    "TIMESTAMP WITHOUT TIME ZONE");
  CHECK(single_column("TIME WITH TIME ZONE").shape.type == sql_type::time);
  // The arguments sit before the tail of the phrase in one dialect and after it
  // in another; both have to reach the column.
  CHECK(single_column("TIMESTAMP(3) WITH TIME ZONE").size == 3);
  CHECK(single_column("CHARACTER VARYING(8)").size == 8);
  column_row const money = single_column("DECIMAL(10,2)");
  CHECK(money.size == 10);
  CHECK(money.decimals == 2);
  CHECK(money.type_name == "DECIMAL");
  CHECK(single_column("NUMERIC").size == 0);
  CHECK(single_column("NUMERIC").decimals == 0);
}

void test_quoted_names_and_comments() {
  ddl_snapshot const snapshot = parse_ddl(R"(
    -- a leading dash comment
    # a hash comment
    /* a block
       comment */
    CREATE TABLE `order``items` (
      `key` INT NOT NULL,
      `index` VARCHAR(4),
      "quoted" DATE,
      PRIMARY KEY (`key`)
    );)");
  CHECK(snapshot.tables.size() == 1);
  CHECK(snapshot.tables[0].ref.name == "order`items");
  ddl_table const& table = snapshot.tables[0];
  // KEY and INDEX name a column of their own in one dialect and open a key list
  // in another; a parenthesis settles which.
  CHECK(table.columns.size() == 3);
  CHECK(column_of(table, "key") != nullptr);
  CHECK(column_of(table, "index") != nullptr);
  CHECK(column_of(table, "quoted") != nullptr);
  CHECK(table.primary_key == std::vector<std::string>{ "key" });
}

void test_default_values() {
  ddl_snapshot const snapshot = parse_ddl(
    "CREATE TABLE t (a VARCHAR(8) DEFAULT 'it''s', "
    "b INT DEFAULT -7, c BIT DEFAULT b'0', d DATETIME DEFAULT now(), "
    "e INT, f TEXT DEFAULT 'x'::text, g INT DEFAULT (NULL), "
    "h TEXT DEFAULT NULL, i VARCHAR(4) DEFAULT '')");
  ddl_table const& table = snapshot.tables[0];
  CHECK(*column_of(table, "a")->default_value == "'it''s'");
  CHECK(*column_of(table, "b")->default_value == "-7");
  CHECK(*column_of(table, "c")->default_value == "b'0'");
  CHECK(*column_of(table, "d")->default_value == "now()");
  CHECK(*column_of(table, "f")->default_value == "'x'::text");
  CHECK(*column_of(table, "g")->default_value == "(NULL)");
  CHECK(*column_of(table, "h")->default_value == "NULL");
  CHECK(*column_of(table, "i")->default_value == "''");
  // A column that says nothing about a default has none to report, which is not
  // the same answer as an explicit NULL.
  CHECK(!column_of(table, "e")->default_value.has_value());
}

void test_nullability_and_keys() {
  ddl_snapshot const snapshot = parse_ddl(
    "CREATE TABLE a (id INT NULL, x INT NOT NULL, PRIMARY KEY (id));"
    "CREATE TABLE b (id INT, seq INT, PRIMARY KEY (seq, id));"
    "CREATE TABLE c (id INT PRIMARY KEY, code VARCHAR(4) UNIQUE)");
  // A primary key is never nullable, whether the column said so or a
  // constraint of its own did.
  ddl_table const& first = snapshot.tables[0];
  CHECK(!column_of(first, "id")->shape.nullable);
  CHECK(!column_of(first, "x")->shape.nullable);
  CHECK(first.primary_key == std::vector<std::string>{ "id" });
  ddl_table const& second = snapshot.tables[1];
  CHECK(second.primary_key ==
    (std::vector<std::string>{ "seq", "id" }));
  CHECK(!column_of(second, "seq")->shape.nullable);
  CHECK(!column_of(second, "id")->shape.nullable);
  // A column-level UNIQUE is a key of its own, named for what it covers.
  ddl_table const& third = snapshot.tables[2];
  CHECK(third.primary_key == std::vector<std::string>{ "id" });
  CHECK(third.indexes.size() == 1);
  CHECK(index_is(third, "unique_code", { "code" }, true));
}

void test_foreign_keys() {
  ddl_snapshot const snapshot = parse_ddl(
    "CREATE TABLE child (\n"
    "  a INT, b INT,\n"
    "  CONSTRAINT pk_pair PRIMARY KEY (a, b),\n"
    "  CONSTRAINT fk_pair FOREIGN KEY (a, b) REFERENCES parent (p, q)\n"
    "    ON DELETE CASCADE ON UPDATE SET NULL DEFERRABLE,\n"
    "  single INT REFERENCES solo (s) NOT NULL\n"
    ");\n"
    "CREATE TABLE later_ref (\n"
    "  owner_id INT,\n"
    "  FOREIGN KEY (owner_id) REFERENCES owner\n"
    ");\n"
    "CREATE TABLE parent (p INT, q INT, PRIMARY KEY (p, q));\n"
    "CREATE TABLE solo (s INT PRIMARY KEY);\n"
    "CREATE TABLE owner (id INT PRIMARY KEY);\n");
  ddl_table const& child = *table_of(snapshot, "child");
  CHECK(child.foreign_keys.size() == 3);
  CHECK(fk_of(child, 0) == "a -> parent(p)");
  CHECK(fk_of(child, 1) == "b -> parent(q)");
  CHECK(fk_of(child, 2) == "single -> solo(s)");
  // A key naming no columns of its own takes the target's primary key, and the
  // target may well be created further down the script.
  ddl_table const& later = *table_of(snapshot, "later_ref");
  CHECK(later.foreign_keys.size() == 1);
  CHECK(fk_of(later, 0) == "owner_id -> owner(id)");
  // A key whose target the script never creates is reported, not guessed: the
  // columns it points at are the one thing a reader of the script cannot know.
  std::vector<std::string> warnings;
  ddl_snapshot const missing = parse_ddl(
    "CREATE TABLE kid (a INT, FOREIGN KEY (a) REFERENCES pa);"
    "CREATE TABLE other (p INT PRIMARY KEY);",
    &warnings);
  CHECK(missing.tables[0].foreign_keys.empty());
  CHECK(mentions(warnings,
    "the FOREIGN KEY on 'kid' references 'pa', whose columns this script does "
    "not name"));
}

void test_foreign_key_size_mismatch_is_refused() {
  CHECK_THROWS(parse_ddl("CREATE TABLE child (a INT, b INT, FOREIGN KEY (a, b)"
    " REFERENCES parent (p));"
    "CREATE TABLE parent (p INT)"), ddl_error);
}

void test_indexes() {
  ddl_snapshot const snapshot = parse_ddl(
    "CREATE TABLE t (a INT, b INT, c VARCHAR(200),"
    " KEY (a), UNIQUE KEY u (b), INDEX (c, a));"
    "CREATE INDEX i_desc ON t (c DESC, a(4) varchar_pattern_ops);"
    "CREATE UNIQUE INDEX i_us ON t USING brin (b);");
  ddl_table const& table = snapshot.tables[0];
  CHECK(table.indexes.size() == 5);
  CHECK(index_is(table, "index_a", { "a" }, false));
  CHECK(index_is(table, "u", { "b" }, true));
  CHECK(index_is(table, "i_us", { "b" }, true));
  // A key the script leaves unnamed still reports the columns it covers.
  CHECK(index_is(table, "index_c_a", { "c", "a" }, false));
  // A prefix length, an operator class or a direction after the column names
  // nothing a mapping asks for.
  CHECK(index_is(table, "i_desc", { "c", "a" }, false));
}

void test_generated_columns() {
  std::vector<std::string> warnings;
  ddl_snapshot const snapshot = parse_ddl(
    "CREATE TABLE t (id BIGINT GENERATED ALWAYS AS IDENTITY NOT NULL,"
    " full_name VARCHAR(80) GENERATED ALWAYS AS (a || b) STORED,"
    " area INT AS (w * h) VIRTUAL, plain INT)",
    &warnings);
  ddl_table const& table = snapshot.tables[0];
  // An identity column is ordinary to read and ordinary to leave out of a
  // write, so it says nothing. An expression column reads but does not take a
  // write, which no member of a struct can say, so it is reported.
  CHECK(table.columns.size() == 4);
  CHECK(!column_of(table, "id")->shape.nullable);
  CHECK(mentions(warnings, "generated column 'full_name'"));
  CHECK(mentions(warnings, "generated column 'area'"));
  CHECK(!mentions(warnings, "generated column 'plain'"));
}

void test_alter_table() {
  std::vector<std::string> warnings;
  ddl_snapshot const snapshot = parse_ddl(
    "CREATE TABLE t (a INT NOT NULL PRIMARY KEY);"
    "ALTER TABLE t ADD COLUMN b VARCHAR(8) NULL AFTER a;"
    "ALTER TABLE t ADD (c INT, d INT);"
    "ALTER TABLE t ADD CONSTRAINT fk_b FOREIGN KEY (b) REFERENCES u (id);"
    "ALTER TABLE t ADD UNIQUE (c);"
    "ALTER TABLE ONLY t DROP COLUMN a, DROP INDEX k,"
    " ALTER COLUMN d TYPE BIGINT;"
    "ALTER TABLE gone ADD COLUMN x INT;",
    &warnings);
  ddl_table const& table = snapshot.tables[0];
  CHECK(table.columns.size() == 4);
  CHECK(column_of(table, "b") != nullptr);
  CHECK(column_of(table, "c") != nullptr);
  CHECK(column_of(table, "d") != nullptr);
  CHECK(table.foreign_keys.size() == 1);
  CHECK(table.indexes.size() == 1);
  CHECK(table.indexes[0].unique);
  // An action that adds nothing changes the shape in a way a script cannot
  // follow, so it is counted rather than applied: a dump repeats the same one
  // thousands of times.
  CHECK(mentions(warnings, "ignored 2 x ALTER TABLE t DROP action"));
  CHECK(mentions(warnings, "ignored ALTER TABLE t ALTER action"));
  CHECK(mentions(warnings, "ALTER TABLE names 'gone'"));
}

void test_statements_that_carry_no_schema_are_quiet() {
  std::vector<std::string> warnings;
  ddl_snapshot const snapshot = parse_ddl(
    "CREATE TABLE t (a INT);"
    "INSERT INTO t VALUES (1); UPDATE t SET a = 2; DELETE FROM t;"
    "SET @@sql_mode = ''; START TRANSACTION; COMMIT;"
    "DROP TABLE IF EXISTS other; SELECT 1;",
    &warnings);
  CHECK(snapshot.tables.size() == 1);
  CHECK(warnings.empty());
}

void test_other_create_statements_are_counted() {
  std::vector<std::string> warnings;
  parse_ddl("CREATE TABLE t (a INT); CREATE VIEW v AS SELECT 1;"
    "CREATE TRIGGER g BEFORE INSERT ON t BEGIN SELECT 1; END;"
    "CREATE INDEX i ON t (a);",
    &warnings);
  CHECK(mentions(warnings, "ignored CREATE VIEW"));
  CHECK(mentions(warnings, "ignored CREATE TRIGGER"));
  CHECK(!mentions(warnings, "ignored CREATE INDEX"));
}

void test_non_value_constraints_are_counted() {
  std::vector<std::string> warnings;
  ddl_snapshot const snapshot = parse_ddl(
    "CREATE TABLE t (a INT, CONSTRAINT ck_a CHECK (a > 0),"
    " FULLTEXT KEY ft (a));"
    "ALTER TABLE t ADD CONSTRAINT ck_b CHECK (a < 100);",
    &warnings);
  CHECK(snapshot.tables[0].columns.size() == 1);
  CHECK(snapshot.tables[0].indexes.empty());
  // A constraint no member of a struct can carry is counted, whichever way the
  // script spells it or however late it arrives.
  CHECK(mentions(warnings,
    "ignored 3 x a non-value constraint on table 't'"));
}

void test_a_table_created_twice_is_reported() {
  std::vector<std::string> warnings;
  ddl_snapshot const snapshot = parse_ddl(
    "CREATE TABLE t (a INT); CREATE TABLE t (b INT);", &warnings);
  CHECK(mentions(warnings, "created twice"));
  CHECK(snapshot.tables.size() == 1);
  CHECK(snapshot.tables[0].columns.size() == 1);
  CHECK(column_of(snapshot.tables[0], "b") != nullptr);
}

void test_rejected_scripts() {
  CHECK_THROWS(parse_ddl("CREATE TABLE t (a INT[])"), ddl_error);
  CHECK_THROWS(parse_ddl("CREATE TABLE t LIKE other"), ddl_error);
  CHECK_THROWS(parse_ddl("CREATE TABLE t (a INT DEFAULT )"), ddl_error);
  CHECK_THROWS(parse_ddl("CREATE TABLE t (a INT NO_SUCH_CLAUSE)"), ddl_error);
  CHECK_THROWS(parse_ddl("CREATE TABLE t (a VARCHAR(8)"), ddl_error);
  CHECK_THROWS(parse_ddl("CREATE TABLE t (a TEXT DEFAULT 'open)"), ddl_error);
  CHECK_THROWS(parse_ddl("CREATE TABLE t (/* unterminated"), ddl_error);
}

void test_narrowing_and_missing_reads() {
  ddl_meta md("USE shop;"
    "CREATE TABLE billing.invoice (id INT NOT NULL PRIMARY KEY);"
    "CREATE TABLE plain (a INT);");
  CHECK(md.database_name() == "shop");
  std::vector<schema_meta::table_row> const all = md.tables("", "");
  CHECK(all.size() == 2);
  CHECK(all[0].schema == "billing");
  // A script that leaves a table unqualified says nothing about where it lives,
  // so a narrowing that names a place cannot exclude it.
  CHECK(md.tables("shop", "billing").size() == 2);
  CHECK(md.tables("", "audit").size() == 1);
  schema_meta::table_ref const absent{ "", "", "nope" };
  CHECK(md.table_columns(absent).empty());
  CHECK(md.primary_key(absent).empty());
  CHECK(md.foreign_keys(absent).empty());
  CHECK(md.indexes(absent).empty());
}

void test_generation_from_a_script() {
  ddl_meta md(R"(
    USE app;
    CREATE TABLE t_user (
      id BIGINT NOT NULL AUTO_INCREMENT,
      name VARCHAR(64) NOT NULL DEFAULT '',
      score DECIMAL(10,2) DEFAULT NULL,
      PRIMARY KEY (id)
    );
    CREATE TABLE t_order (
      id BIGINT NOT NULL,
      user_id BIGINT NOT NULL,
      created DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY (id),
      KEY idx_user (user_id),
      CONSTRAINT fk_user FOREIGN KEY (user_id) REFERENCES t_user (id)
    );)");
  read_options opts;
  opts.tables = { "t_order", "t_user" };
  std::vector<std::string> warnings;
  schema_model model = read_schema(md, opts, &warnings);
  model.name = "app";
  CHECK(warnings.empty());
  // The selection is in requested order, with the names the script spells.
  CHECK(model.tables.size() == 2);
  CHECK(model.tables[0].name == "t_order");
  CHECK(model.tables[1].name == "t_user");
  CHECK(model.tables[0].columns[2].default_value == "CURRENT_TIMESTAMP");

  generated_output out = generate_header(model, gen_config{});
  CHECK(out.warnings.empty());
  CHECK(out.text.find("namespace app {") != std::string::npos);
  CHECK(out.text.find("struct TUser {") != std::string::npos);
  CHECK(out.text.find(".column(\"name\", &TUser::name)") !=
    std::string::npos);
  CHECK(out.text.find("DECIMAL(10,2) DEFAULT NULL") != std::string::npos);
  CHECK(out.text.find("FK: user_id -> t_user(id)") != std::string::npos);
  CHECK(out.text.find("index: idx_user (user_id)") != std::string::npos);
  CHECK(out.text.find("register_app_schema(uniorm::orm& registry)") !=
    std::string::npos);
}

}  // namespace

void test_gen_ddl() {
  test_number_and_boolean_types();
  test_text_and_binary_types();
  test_date_time_and_decimal_arguments();
  test_quoted_names_and_comments();
  test_default_values();
  test_nullability_and_keys();
  test_foreign_keys();
  test_foreign_key_size_mismatch_is_refused();
  test_indexes();
  test_generated_columns();
  test_alter_table();
  test_statements_that_carry_no_schema_are_quiet();
  test_other_create_statements_are_counted();
  test_non_value_constraints_are_counted();
  test_a_table_created_twice_is_reported();
  test_rejected_scripts();
  test_narrowing_and_missing_reads();
  test_generation_from_a_script();
}
