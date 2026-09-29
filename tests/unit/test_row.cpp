#include <memory>
#include <string>
#include <vector>

#include <uniorm/row.hpp>

#include "check.hpp"

using namespace uniorm;

namespace {

row make_row() {
  auto names = column_names::create(
    std::vector<std::string>{ "id", "name", "age", "data" });
  std::vector<sql_value> values;
  values.push_back(std::int64_t{ 42 });
  values.push_back(std::string{ "alice" });
  values.push_back(std::monostate{});
  values.push_back(std::vector<std::byte>{ std::byte{ 1 }, std::byte{ 2 } });
  return row(std::move(names), std::move(values));
}

}  // namespace

void test_row() {
  std::printf("  test_row: creating row...\n");
  std::fflush(stdout);
  row r = make_row();
  std::printf("  test_row: row created\n");
  std::fflush(stdout);
  std::printf("  test_row: checking size...\n");
  std::fflush(stdout);
  CHECK(r.size() == 4);

  std::printf("  test_row: get by name...\n");
  std::fflush(stdout);
  CHECK(r.get<std::int64_t>("id") == 42);
  CHECK(r.get<std::int32_t>("id") == 42);  // tolerant numeric narrowing
  CHECK(r.get<std::string>("name") == "alice");
  CHECK(r.is_null("age"));
  CHECK(!r.is_null("id"));
  CHECK(r.get<std::vector<std::byte>>("data").size() == 2);

  std::printf("  test_row: get by index...\n");
  std::fflush(stdout);
  CHECK(r.get<std::int64_t>(0) == 42);
  CHECK(r.get<std::string>(1) == "alice");

  std::printf("  test_row: exception tests...\n");
  std::fflush(stdout);
  CHECK_THROWS(r.at("missing"), column_not_found);
  CHECK_THROWS(r.at(99), column_not_found);
  CHECK_THROWS(r.get<std::string>("id"), type_mismatch);
  CHECK_THROWS(r.get<timestamp>("name"), type_mismatch);

  std::printf("  test_row: narrowing test...\n");
  std::fflush(stdout);
  // out-of-range narrowing must throw
  std::vector<sql_value> values{ std::int64_t{ 5'000'000'000LL } };
  auto names = column_names::create(std::vector<std::string>{ "big" });
  row big(std::move(names), std::move(values));
  std::printf("  test_row: big row created\n");
  std::fflush(stdout);
  CHECK_THROWS(big.get<std::int32_t>("big"), type_mismatch);
  std::printf("  test_row: narrowing test done\n");
  std::fflush(stdout);

  std::printf("  test_row: decimal tests...\n");
  std::fflush(stdout);
  // a DECIMAL column arrives as its exact literal text
  std::vector<sql_value> text{ std::string{ "12345678901234.5678" },
    std::string{ "42" }, std::string{ "alice" } };
  auto text_names = column_names::create(
    std::vector<std::string>{ "amount", "whole", "label" });
  row dec(std::move(text_names), std::move(text));
  std::printf("  test_row: dec row created\n");
  std::fflush(stdout);
  CHECK(dec.get<std::string>("amount") == "12345678901234.5678");
  CHECK(dec.get<double>("amount") > 12345678901234.56);
  CHECK(dec.get<double>("amount") < 12345678901234.57);
  CHECK(dec.get<std::int64_t>("whole") == 42);
  CHECK_THROWS(dec.get<std::int64_t>("amount"), type_mismatch);
  CHECK_THROWS(dec.get<std::int32_t>("amount"), type_mismatch);
  CHECK_THROWS(dec.get<std::int64_t>("label"), type_mismatch);
  CHECK_THROWS(dec.get<bool>("whole"), type_mismatch);
  std::printf("  test_row: done\n");
  std::fflush(stdout);
}
