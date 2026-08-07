#include <string>

#include <uniorm/params.hpp>

#include "check.hpp"

using namespace uniorm;

void test_params() {
  params p(18, "alice", std::string{ "bob" }, 3.25f, nullptr, std::int64_t{ 7 },
    std::vector<std::byte>{ std::byte{ 9 } });

  CHECK(p.size() == 7);
  CHECK(std::holds_alternative<std::int32_t>(p.at(0)));
  CHECK(std::get<std::int32_t>(p.at(0)) == 18);
  CHECK(std::get<std::string>(p.at(1)) == "alice");
  CHECK(std::get<std::string>(p.at(2)) == "bob");
  CHECK(std::holds_alternative<double>(p.at(3)));
  CHECK(std::holds_alternative<std::monostate>(p.at(4)));
  CHECK(std::holds_alternative<std::int64_t>(p.at(5)));
  CHECK(std::holds_alternative<std::vector<std::byte>>(p.at(6)));

  // unsigned 32-bit goes to int64 to avoid sign issues
  params pu(42u);
  CHECK(std::holds_alternative<std::int64_t>(pu.at(0)));

  // timestamp survives
  timestamp now = std::chrono::system_clock::now();
  params pt(now);
  CHECK(std::get<timestamp>(pt.at(0)) == now);
}
