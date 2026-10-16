#include "../unit/check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <uniorm/connection.hpp>
#include <uniorm/converter.hpp>
#include <uniorm/decimal.hpp>
#include "uniorm/mapping/registry.hpp"
#include "uniorm/builder/builder.hpp"

// The checked-in golden header was produced by uniorm-gen against the
// fixture tables below and the override file beside it; regenerate it with
// this one command, wrapped here only for width:
//   uniorm-gen --dsn=<dsn> --user=<u> --password=<p>
//     --tables=uniorm_gen_user,uniorm_gen_order --name=gen_it
//     --config=tests/integration/golden/gen_it.toml
//     --out=tests/integration/golden
// The macros expand to quoted string literals provided by CMake. The script
// beside the golden holds the same tables for --input-ddl, so the offline
// source owes this test the same code without ever opening a connection.

// The domain type the override file names. A generated header reaches a
// converter-backed member through uniorm::converter<T>, so the
// specialization has to be declared before the header is included.
enum class order_state { unpaid, paid, shipped };

template <>
struct uniorm::converter<order_state> {
  using db_type = std::string;

  static void to_db(order_state const& s, std::string& out) {
    if (s == order_state::paid) {
      out = "paid";
    } else if (s == order_state::shipped) {
      out = "shipped";
    } else {
      out = "unpaid";
    }
  }

  static order_state from_db(std::string const& v) {
    if (v == "paid") {
      return order_state::paid;
    }
    if (v == "shipped") {
      return order_state::shipped;
    }
    return order_state::unpaid;
  }
};

#include UNIORM_GEN_GOLDEN

using namespace uniorm;

namespace {

char const* k_user_table = "uniorm_gen_user";
char const* k_order_table = "uniorm_gen_order";

void prepare_schema(connection& conn) {
  conn.execute_update(std::string("DROP TABLE IF EXISTS ") + k_order_table);
  conn.execute_update(std::string("DROP TABLE IF EXISTS ") + k_user_table);
  conn.execute_update(std::string("CREATE TABLE ") + k_user_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " name VARCHAR(64) NOT NULL,"
                      " age INT NULL,"
                      " created TIMESTAMP NULL)");
  conn.execute_update(std::string("CREATE TABLE ") + k_order_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " user_id BIGINT NOT NULL,"
                      " amount DECIMAL(10,2) NOT NULL,"
                      " note VARCHAR(128) NULL,"
                      " CONSTRAINT fk_gen_order_user FOREIGN KEY (user_id)"
                      " REFERENCES " +
                      k_user_table +
                      " (id))");
  // The generator reads this index back out of the driver's catalog, and the
  // line that creates it is the only one the MySQL table syntax owns.
  conn.execute_update(std::string("CREATE INDEX idx_gen_order_user ON ") +
                      k_order_table + " (user_id)");
}

void drop_schema(connection& conn) {
  conn.execute_update(std::string("DROP TABLE IF EXISTS ") + k_order_table);
  conn.execute_update(std::string("DROP TABLE IF EXISTS ") + k_user_table);
}

// Compile -> register -> validate(strict) on the checked-in golden header.
void test_golden(std::string_view conn_string) {
  orm db(conn_string);
  gen_it::register_gen_it_schema(db);
  CHECK(db.size() == 2);
  db.validate(validation_mode::strict);
  CHECK(db.query<gen_it::UniormGenUser>().count() == 0);

  // The generated entity carries both a converter-backed column and a
  // decimal_t one, so a write and a materialized read here cover the whole
  // generated mapping.
  gen_it::UniormGenUser user;
  user.id = 1;
  user.name = "ada";
  CHECK(db.insert(std::vector<gen_it::UniormGenUser>{ user }) == 1);
  gen_it::UniormGenOrder order;
  order.id = 1;
  order.userId = 1;
  order.amount = decimal_t::from_literal("10.50");
  order.note = order_state::shipped;
  CHECK(db.insert(std::vector<gen_it::UniormGenOrder>{ order }) == 1);

  auto back = db.query<gen_it::UniormGenOrder>().one();
  CHECK(back.has_value());
  CHECK(back->amount == decimal_t::from_literal("10.5"));
  CHECK(back->amount.to_literal() == "10.50");  // the column's scale
  CHECK(back->note && *back->note == order_state::shipped);
}

std::string read_normalized(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string text = buf.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

// Metadata that leaves a trace nowhere else: a foreign key and a secondary
// index appear in no line of generated code, so only these comments show the
// extraction found them. Everything else -- the primary key, the member types
// -- is code, and code is compared.
char const* const kMetadataMarkers[] = {
  "FK: user_id -> uniorm_gen_user(id)",
  "index: idx_gen_order_user (user_id)",
};

// The comments the tool writes carry the connector's spelling of a type name
// and the server's way of storing a default, which makes them differ between
// the pairs this suite runs under and mean nothing to anything that compiles.
// A trailing one goes with the line's own text. A line that is nothing but a
// comment reports metadata the two sources do not promise to agree on: a live
// catalog lists a primary key as an index, a script never says so.
std::string without_comments(std::string const& text) {
  std::string out;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    auto const comment = line.find("//");
    if (comment == std::string::npos) {
      out += line;
      out += '\n';
      continue;
    }
    std::string const head = line.substr(0, comment);
    if (head.find_first_not_of(" \t") == std::string::npos) {
      continue;
    }
    out += head;
    out += '\n';
  }
  return out;
}

// Runs the tool over one schema source and holds what it wrote to the golden:
// the metadata comments have to be there, the code has to be that code. A
// failure names the out_dir rather than the source, which carries a password.
void require_matches_golden(
  std::string const& source, std::string const& out_dir) {
  std::string cmd = std::string(UNIORM_GEN_PATH) + " " + source +
                    " --out=" + out_dir +
                    " --tables=uniorm_gen_user,uniorm_gen_order"
                    " --name=gen_it --config=" UNIORM_GEN_CONFIG " 2>&1";
  int rc = std::system(cmd.c_str());
  CHECK(rc == 0);

  std::string text = read_normalized(out_dir + "/gen_it_schema.hpp");
  CHECK(!text.empty());
  for (char const* marker : kMetadataMarkers) {
    if (text.find(marker) == std::string::npos) {
      std::printf("FAIL extraction lost metadata: %s\n", marker);
      ++uniorm::test::failure_count();
    }
  }
  std::string generated = without_comments(text);
  std::string golden = without_comments(read_normalized(UNIORM_GEN_GOLDEN));
  if (generated == golden) {
    return;
  }
  std::size_t pos = 0;
  while (pos < generated.size() && pos < golden.size() &&
         generated[pos] == golden[pos]) {
    ++pos;
  }
  std::printf("FAIL %s differs from golden at byte %zu\n", out_dir.c_str(),
    pos);
  std::printf("  generated: %s\n",
    generated.substr(pos > 40 ? pos - 40 : 0, 80).c_str());
  std::printf("  golden:    %s\n",
    golden.substr(pos > 40 ? pos - 40 : 0, 80).c_str());
  ++uniorm::test::failure_count();
}

// Live extraction: the fixture tables this test just created.
void test_generate_matches_golden(std::string const& conn_string) {
  require_matches_golden("--connection-string=\"" + conn_string + "\"",
    UNIORM_GEN_OUT_DIR);
}

// The same server reached by handing the tool the pieces instead of a string:
// the tool spells nothing, so this run matching the golden is the backend's
// own spelling working end to end, catalog read included.
void test_generate_matches_golden_by_pieces(std::string const& dsn,
  std::string const& user, std::string const& pwd) {
  std::string const pieces = "--dsn=\"" + dsn + "\" --user=\"" + user +
    "\" --password=\"" + pwd + "\"";
  require_matches_golden(pieces, std::string(UNIORM_GEN_OUT_DIR) + "/pieces");
}

// Offline extraction: the same tables as a script, so a golden header is
// reproducible without a server, and the two sources owe each other nothing
// but the code.
void test_generate_from_a_script_matches_golden() {
  require_matches_golden("--input-ddl=" UNIORM_GEN_SQL,
    std::string(UNIORM_GEN_OUT_DIR) + "/ddl");
}

struct run_result {
  int code;
  std::string text;
};

// One call of the binary, with everything it printed. std::system gives the
// command processor's exit status, which is a wait status here and the exit
// code itself on Windows.
run_result run_gen(std::string const& args, std::string const& log_path) {
  std::string const cmd = std::string(UNIORM_GEN_PATH) + " " + args +
                          " >" + log_path + " 2>&1";
  int const raw = std::system(cmd.c_str());
  run_result result;
#ifdef _WIN32
  result.code = raw;
#else
  result.code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
  result.text = read_normalized(log_path);
  return result;
}

void require_code(std::string const& item, int code, int expected) {
  if (code == expected) {
    return;
  }
  std::printf("FAIL %s: exit %d, expected %d\n", item.c_str(), code,
    expected);
  ++uniorm::test::failure_count();
}

void require_text(std::string const& item, std::string const& text,
  std::string const& needle) {
  if (text.find(needle) != std::string::npos) {
    return;
  }
  std::printf("FAIL %s: output has no '%s'\n", item.c_str(), needle.c_str());
  std::printf("  output: %s\n", text.c_str());
  ++uniorm::test::failure_count();
}

std::string write_script(std::string const& dir, std::string const& name,
  std::string const& body) {
  std::filesystem::create_directories(dir);
  std::string const path = dir + "/" + name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
  return path;
}

// The CLI's own contract, checked against the binary rather than the library:
// which sources may appear together, what a rejected script says and on which
// line, and what a run that fails leaves behind. An in-process test cannot see
// any of it -- exit codes, the shape of stderr and "wrote nothing" are this
// program's interface.
void test_cli_contract() {
  std::string const root(UNIORM_GEN_OUT_DIR);
  std::string const log = root + "/cli.log";
  std::string const out = root + "/cli";
  std::string const rejected = root + "/cli_rejected";
  std::error_code ignored;
  std::filesystem::remove_all(out, ignored);
  std::filesystem::remove_all(rejected, ignored);

  run_result good = run_gen("--input-ddl=" UNIORM_GEN_SQL " --out=" + out +
    " --name=gen_it", log);
  require_code("--input-ddl run", good.code, 0);
  // The tool prints the path it wrote, in this platform's separators, so the
  // line is what is checked and the file itself is looked for by the API.
  require_text("--input-ddl run", good.text, "gen_it_schema.hpp");
  CHECK(std::filesystem::exists(out + "/gen_it_schema.hpp"));

  run_result help = run_gen("--help", log);
  require_code("--help", help.code, 0);
  require_text("--help", help.text, "usage: uniorm-gen");

  // Two sources, then none: the same rule the tool prints its usage for.
  for (std::string const& args :
      { "--input-ddl=" UNIORM_GEN_SQL " --dsn=anything --out=" + rejected,
          "--out=" + rejected }) {
    run_result both = run_gen(args, log);
    require_code("two sources refused", both.code, 2);
    require_text("two sources refused", both.text,
      "exactly one of --dsn / --connection-string / --input-ddl is required");
  }

  run_result no_out = run_gen("--input-ddl=" UNIORM_GEN_SQL, log);
  require_code("--out required", no_out.code, 2);
  require_text("--out required", no_out.text, "--out=<dir> is required");

  run_result absent = run_gen(
    "--input-ddl=" + root + "/no_such_file.sql --out=" + rejected, log);
  require_code("missing script", absent.code, 1);
  require_text("missing script", absent.text, "cannot open DDL file");

  // A script the parser refuses names the line it gave up on, and a refused
  // run writes no header at all.
  std::string const arrays = write_script(root, "arrays.sql",
    "CREATE TABLE t (\n  a INT[]\n);\n");
  run_result array = run_gen("--input-ddl=" + arrays + " --out=" + rejected,
    log);
  require_code("array column", array.code, 1);
  require_text("array column", array.text, "ddl: line 2:");
  require_text("array column", array.text, "carries no arrays");

  run_result missing_table = run_gen("--input-ddl=" UNIORM_GEN_SQL
    " --out=" + rejected + " --tables=no_such_table", log);
  require_code("unknown --tables name", missing_table.code, 1);
  require_text("unknown --tables name", missing_table.text,
    "table not found: no_such_table");

  CHECK(!std::filesystem::exists(rejected));

  // A type the mapping has no entry for is a warning and a text column, not a
  // failure: the live read answers the same way.
  std::string const odd = write_script(root, "odd_type.sql",
    "CREATE TABLE t (\n  a WIDGET NOT NULL\n);\n");
  run_result odd_run = run_gen(
    "--input-ddl=" + odd + " --out=" + out + " --name=odd_type", log);
  require_code("unmapped type", odd_run.code, 0);
  require_text("unmapped type", odd_run.text,
    "warning: t.a: unmapped SQL type 'WIDGET', using std::string");

  // What a script states that a mapping cannot use is counted, not failed.
  std::string const dropped = write_script(root, "dropped.sql",
    "CREATE TABLE t (\n  a INT NOT NULL\n);\n"
    "ALTER TABLE t DROP COLUMN a;\n");
  run_result dropped_run = run_gen("--input-ddl=" + dropped +
    " --out=" + out + " --name=dropped", log);
  require_code("ignored action", dropped_run.code, 0);
  require_text("ignored action", dropped_run.text,
    "warning: ignored ALTER TABLE t DROP action");
}

}  // namespace

// A DSN this job cannot reach is a skip, but an offline leg that already
// failed is not: 77 would have ctest record the run as a pass.
static int skip_or_report(std::string const& reason, int counted) {
  if (counted == 0) {
    std::printf("skip: %s\n", reason.c_str());
    return 77;
  }
  std::printf("%d gen e2e test(s) failed; the database legs did not run\n",
    counted);
  return 1;
}

int main() {
  // These two legs open no connection, so they go first and stand on their own.
  try {
    test_generate_from_a_script_matches_golden();
    test_cli_contract();
  } catch (std::exception const& e) {
    std::printf("FATAL: unexpected exception: %s\n", e.what());
    ++uniorm::test::failure_count();
  }
  int const offline_failures = uniorm::test::failure_count();

  char const* dsn_env = std::getenv("UNIORM_IT_DSN");
  char const* user_env = std::getenv("UNIORM_IT_USER");
  char const* pwd_env = std::getenv("UNIORM_IT_PWD");
  if (!dsn_env || !*dsn_env || !user_env || !*user_env || !pwd_env ||
      !*pwd_env) {
    return skip_or_report("set UNIORM_IT_DSN / UNIORM_IT_USER / "
      "UNIORM_IT_PWD to run gen e2e tests", offline_failures);
  }
  std::string dsn = dsn_env;
  std::string user = user_env;
  std::string pwd = pwd_env;
  std::string conn_string = "DSN=" + dsn + ";UID=" + user + ";PWD=" + pwd;

  try {
    connection probe(conn_string);
  } catch (std::exception const& e) {
    return skip_or_report("cannot connect to DSN '" + dsn + "': " + e.what(),
      offline_failures);
  }

  try {
    connection conn(conn_string);
    prepare_schema(conn);
    test_golden(conn_string);
    test_generate_matches_golden(conn_string);
    test_generate_matches_golden_by_pieces(dsn, user, pwd);
    drop_schema(conn);
  } catch (std::exception const& e) {
    std::printf("FATAL: unexpected exception: %s\n", e.what());
    ++uniorm::test::failure_count();
  }

  int failures = uniorm::test::failure_count();
  if (failures == 0) {
    std::printf("all gen e2e tests passed\n");
    return 0;
  }
  std::printf("%d gen e2e test(s) failed\n", failures);
  return 1;
}
