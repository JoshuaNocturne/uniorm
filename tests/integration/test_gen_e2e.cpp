#include "../unit/check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "uniorm/connection.hpp"
#include "uniorm/mapping/registry.hpp"
#include "uniorm/query/builder.hpp"

// The checked-in golden header was produced by uniorm-gen against the
// fixture tables below; regenerate it with:
//   uniorm-gen --dsn=<dsn> --user=<u> --password=<p> \
//     --tables=uniorm_gen_user,uniorm_gen_order --name=gen_it \
//     --out=tests/integration/golden
// (assumes the default UNIORM_DECIMAL_DEFAULT=string build).
// The macros expand to quoted string literals provided by CMake.
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
                      " created DATETIME NULL)");
  conn.execute_update(std::string("CREATE TABLE ") + k_order_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " user_id BIGINT NOT NULL,"
                      " amount DECIMAL(10,2) NOT NULL,"
                      " note VARCHAR(128) NULL,"
                      " CONSTRAINT fk_gen_order_user FOREIGN KEY (user_id)"
                      " REFERENCES " +
                      k_user_table +
                      " (id),"
                      " KEY idx_gen_order_user (user_id))");
}

void drop_schema(connection& conn) {
  conn.execute_update(std::string("DROP TABLE IF EXISTS ") + k_order_table);
  conn.execute_update(std::string("DROP TABLE IF EXISTS ") + k_user_table);
}

// Compile -> register -> validate(strict) on the checked-in golden header.
void test_golden(connection& conn) {
  orm registry;
  gen_it::register_gen_it_schema(registry);
  CHECK(registry.size() == 2);
  registry.validate(conn, validation_mode::strict);
  CHECK(conn.query(registry).of<gen_it::UniormGenUser>().count() == 0);
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

// Live extraction: run the tool against the fixture tables and compare the
// result byte-for-byte with the checked-in golden.
void test_generate_matches_golden(std::string const& conn_string) {
  std::string cmd = std::string(UNIORM_GEN_PATH) + " --connection-string=\"" +
                    conn_string + "\" --out=" + UNIORM_GEN_OUT_DIR +
                    " --tables=uniorm_gen_user,uniorm_gen_order"
                    " --name=gen_it 2>&1";
  int rc = std::system(cmd.c_str());
  CHECK(rc == 0);

  std::string generated =
    read_normalized(std::string(UNIORM_GEN_OUT_DIR) + "/gen_it_schema.hpp");
  std::string golden = read_normalized(UNIORM_GEN_GOLDEN);
  CHECK(!generated.empty());
  if (generated != golden) {
    std::size_t pos = 0;
    while (pos < generated.size() && pos < golden.size() &&
           generated[pos] == golden[pos]) {
      ++pos;
    }
    std::printf("FAIL generated output differs from golden at byte %zu\n", pos);
    std::printf("  generated: %s\n",
      generated.substr(pos > 40 ? pos - 40 : 0, 80).c_str());
    std::printf(
      "  golden:    %s\n", golden.substr(pos > 40 ? pos - 40 : 0, 80).c_str());
    ++uniorm::test::failure_count();
  }
}

}  // namespace

int main() {
  char const* dsn_env = std::getenv("UNIORM_IT_DSN");
  char const* user_env = std::getenv("UNIORM_IT_USER");
  char const* pwd_env = std::getenv("UNIORM_IT_PWD");
  if (!dsn_env || !*dsn_env || !user_env || !*user_env || !pwd_env ||
      !*pwd_env) {
    std::printf(
      "skip: set UNIORM_IT_DSN / UNIORM_IT_USER / UNIORM_IT_PWD to run "
      "gen e2e tests\n");
    return 77;
  }
  std::string dsn = dsn_env;
  std::string user = user_env;
  std::string pwd = pwd_env;
  std::string conn_string = "DSN=" + dsn + ";UID=" + user + ";PWD=" + pwd;

  try {
    connection probe(conn_string);
  } catch (std::exception const& e) {
    std::printf(
      "skip: cannot connect to DSN '%s': %s\n", dsn.c_str(), e.what());
    return 77;
  }

  try {
    connection conn(conn_string);
    prepare_schema(conn);
    test_golden(conn);
    test_generate_matches_golden(conn_string);
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
