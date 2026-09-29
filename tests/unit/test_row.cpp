#include <string>
#include <vector>

#include <uniorm/row.hpp>

#include "check.hpp"

using namespace uniorm;

namespace {

row make_row() {
  std::vector<sql_value> values;
  values.push_back(std::int64_t{ 42 });
  values.push_back(std::string{ "alice" });
  values.push_back(std::monostate{});
  values.push_back(std::vector<std::byte>{ std::byte{ 1 }, std::byte{ 2 } });
  return row({"id", "name", "age", "data"}, std::move(values));
}

}  // namespace

void test_row() {
  std::printf("  1\n");
  std::fflush(stdout);
  row r = make_row();
  std::printf("  2\n");
  std::fflush(stdout);

  CHECK(r.size() == 4);
  CHECK(r.get<std::int64_t>("id") == 42);
  CHECK(r.get<std::int32_t>("id") == 42);
  CHECK(r.get<std::string>("name") == "alice");
  CHECK(r.is_null("age"));
  CHECK(!r.is_null("id"));
  CHECK(r.get<std::vector<std::byte>>("data").size() == 2);
  CHECK(r.get<std::int64_t>(0) == 42);
  CHECK(r.get<std::string>(1) == "alice");
  std::printf("  3\n");
  std::fflush(stdout);

  CHECK_THROWS(r.at("missing"), column_not_found);
  CHECK_THROWS(r.at(99), column_not_found);
  CHECK_THROWS(r.get<std::string>("id"), type_mismatch);
  CHECK_THROWS(r.get<timestamp>("name"), type_mismatch);
  std::printf("  4\n");
  std::fflush(stdout);

  {
    std::vector<sql_value> values{ std::int64_t{ 5'000'000'000LL } };
    row big({"big"}, std::move(values));
    CHECK_THROWS(big.get<std::int32_t>("big"), type_mismatch);
    std::printf("  5\n");
    std::fflush(stdout);
  }
  std::printf("  6\n");
  std::fflush(stdout);

  {
    std::vector<sql_value> text{ std::string{ "12345678901234.5678" },
      std::string{ "42" }, std::string{ "alice" } };
    row dec({"amount", "whole", "label"}, std::move(text));
    CHECK(dec.get<std::string>("amount") == "12345678901234.5678");
    CHECK(dec.get<double>("amount") > 12345678901234.56);
    CHECK(dec.get<double>("amount") < 12345678901234.57);
    CHECK(dec.get<std::int64_t>("whole") == 42);
    CHECK_THROWS(dec.get<std::int64_t>("amount"), type_mismatch);
    CHECK_THROWS(dec.get<std::int32_t>("amount"), type_mismatch);
    CHECK_THROWS(dec.get<std::int64_t>("label"), type_mismatch);
    CHECK_THROWS(dec.get<bool>("whole"), type_mismatch);
    std::printf("  7\n");
    std::fflush(stdout);
  }
  std::printf("  8\n");
  std::fflush(stdout);
}
