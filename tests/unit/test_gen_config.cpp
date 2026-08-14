#include "check.hpp"

#include <string>

#include "config.hpp"

using namespace uniorm::gen;

namespace {

// The sample from design.md section 6.4.
char const* k_sample = R"toml(
# global SQL type -> C++ type overrides
[types]
"NUMERIC(10,2)" = "std::int64_t"
"TIMESTAMP" = "uniorm::timestamp"

[tables.t_user]
class = "User"
skip = false

[tables.t_user.columns.status]
cpp_type = "std::string"
converter = "status_converter"

[tables.t_audit]
skip = true
)toml";

void test_parse_sample() {
  gen_config cfg = parse_config(k_sample);
  CHECK(cfg.type_overrides.size() == 2);
  CHECK(cfg.type_overrides["NUMERIC(10,2)"] == "std::int64_t");
  CHECK(cfg.type_overrides["TIMESTAMP"] == "uniorm::timestamp");

  CHECK(cfg.tables.size() == 2);
  table_config const& user = cfg.tables.at("t_user");
  CHECK(user.class_name && *user.class_name == "User");
  CHECK(!user.skip);
  CHECK(user.columns.size() == 1);
  column_override const& status = user.columns.at("status");
  CHECK(status.cpp_type && *status.cpp_type == "std::string");
  CHECK(status.converter && *status.converter == "status_converter");

  CHECK(cfg.tables.at("t_audit").skip);
}

void test_parse_quoted_table_name() {
  gen_config cfg = parse_config("[tables.\"my.table\"]\nclass = \"X\"\n");
  CHECK(cfg.tables.at("my.table").class_name &&
        *cfg.tables.at("my.table").class_name == "X");
}

void test_parse_errors() {
  CHECK_THROWS(parse_config("key = \"v\"\n"), config_error);
  CHECK_THROWS(parse_config("[unknown]\nk = \"v\"\n"), config_error);
  CHECK_THROWS(parse_config("[tables.t]\nbogus = \"v\"\n"), config_error);
  CHECK_THROWS(
    parse_config("[tables.t.columns.c]\nbogus = \"v\"\n"), config_error);
  CHECK_THROWS(parse_config("[tables.t]\nskip = \"yes\"\n"), config_error);
  CHECK_THROWS(parse_config("[tables.t]\nclass = User\n"), config_error);
  CHECK_THROWS(parse_config("[types]\n\"X\" = \"unterminated\n"), config_error);

  try {
    parse_config("\n\n[tables.t]\nbogus = 1\n");
  } catch (config_error const& e) {
    CHECK(std::string(e.what()).find("line 4") != std::string::npos);
  }
}

}  // namespace

void test_gen_config() {
  test_parse_sample();
  test_parse_quoted_table_name();
  test_parse_errors();
}
