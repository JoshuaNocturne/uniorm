#include <memory>
#include <string>
#include <vector>

#include <uniorm/row.hpp>

#include "check.hpp"

using namespace uniorm;

namespace {

row make_row() {
  auto names = std::make_shared<std::vector<std::string>>(
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
  row r = make_row();
  CHECK(r.size() == 4);

  CHECK(r.get<std::int64_t>("id") == 42);
  CHECK(r.get<std::int32_t>("id") == 42);  // tolerant numeric narrowing
  CHECK(r.get<std::string>("name") == "alice");
  CHECK(r.is_null("age"));
  CHECK(!r.is_null("id"));
  CHECK(r.get<std::vector<std::byte>>("data").size() == 2);

  CHECK(r.get<std::int64_t>(0) == 42);
  CHECK(r.get<std::string>(1) == "alice");

  CHECK_THROWS(r.at("missing"), column_not_found);
  CHECK_THROWS(r.at(99), column_not_found);
  CHECK_THROWS(r.get<std::string>("id"), type_mismatch);
  CHECK_THROWS(r.get<timestamp>("name"), type_mismatch);

  // out-of-range narrowing must throw
  std::vector<sql_value> values{ std::int64_t{ 5'000'000'000LL } };
  auto names = std::make_shared<std::vector<std::string>>(
    std::vector<std::string>{ "big" });
  row big(std::move(names), std::move(values));
  CHECK_THROWS(big.get<std::int32_t>("big"), type_mismatch);
}
