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
  std::printf("  A: default row\n");
  std::fflush(stdout);
  {
    row empty;
    std::printf("  A1: size=%zu\n", empty.size());
    std::fflush(stdout);
  }
  std::printf("  B: make_row\n");
  std::fflush(stdout);
  {
    row r = make_row();
    std::printf("  B1: size=%zu\n", r.size());
    std::fflush(stdout);
    CHECK(r.size() == 4);
    CHECK(r.get<std::int64_t>("id") == 42);
    std::printf("  B2: checks done\n");
    std::fflush(stdout);
  }
  std::printf("  C: done\n");
  std::fflush(stdout);
}
