#include "check.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <uniorm/error.hpp>
#include <uniorm/mapping/registry.hpp>

using namespace uniorm;

namespace {

struct Account {
  std::int64_t id;
  std::string owner;
  std::optional<std::string> nickname;
  std::int32_t balance = 0;
  std::int32_t cached = 0;
};

}  // namespace

void test_registry() {
  orm registry;
  registry.map<Account>("accounts")
    .primary_key("id", &Account::id)
    .column("owner", &Account::owner)
    .column("nickname", &Account::nickname)
    .column("balance", &Account::balance)
    .ignore(&Account::cached);

  CHECK(registry.size() == 1);
  auto const& meta = registry.meta<Account>();
  CHECK(meta.table == "accounts");
  CHECK(meta.columns.size() == 4);
  CHECK(meta.columns[0].is_primary_key);
  CHECK(!meta.columns[0].nullable);
  CHECK(meta.columns[2].nullable);  // optional member
  CHECK(meta.ignored.size() == 1);

  CHECK(meta.column_name(make_member_key(&Account::owner)) == "owner");
  CHECK_THROWS(
    meta.column_name(make_member_key(&Account::cached)), mapping_error);

  struct Other {
    int x;
  };
  CHECK_THROWS(meta.column_name(make_member_key(&Other::x)), mapping_error);

  auto names = std::make_shared<std::vector<std::string>>(
    std::vector<std::string>{ "id", "owner", "nickname", "balance" });
  {
    row r(
      names, { sql_value(std::int64_t{ 42 }), sql_value(std::string("alice")),
               sql_value(std::monostate{}), sql_value(std::int64_t{ 100 }) });
    Account acc{};
    meta.populate(&acc, r);
    CHECK(acc.id == 42);
    CHECK(acc.owner == "alice");
    CHECK(!acc.nickname.has_value());
    CHECK(acc.balance == 100);  // int64 narrowed to int32
    CHECK(acc.cached == 0);  // ignored member untouched
  }
  {
    Account acc{};
    acc.id = 9;
    acc.owner = "carol";
    acc.nickname = std::nullopt;
    acc.balance = 55;
    CHECK(meta.columns[0].read(&acc) == sql_value(std::int64_t{ 9 }));
    CHECK(meta.columns[1].read(&acc) == sql_value(std::string("carol")));
    CHECK(is_null(meta.columns[2].read(&acc)));  // empty optional -> NULL
    CHECK(meta.columns[3].read(&acc) == sql_value(std::int32_t{ 55 }));
    acc.nickname = "cc";
    CHECK(meta.columns[2].read(&acc) == sql_value(std::string("cc")));
  }
  {
    row r(
      names, { sql_value(std::int64_t{ 7 }), sql_value(std::string("bob")),
               sql_value(std::string("bobby")), sql_value(std::int32_t{ 1 }) });
    Account acc{};
    meta.populate(&acc, r);
    CHECK(acc.nickname.has_value() && *acc.nickname == "bobby");
  }
  {
    auto short_names = std::make_shared<std::vector<std::string>>(
      std::vector<std::string>{ "id", "owner", "balance" });
    row r(
      short_names, { sql_value(std::int64_t{ 1 }), sql_value(std::string("x")),
                     sql_value(std::int32_t{ 2 }) });
    Account acc{};
    CHECK_THROWS(meta.populate(&acc, r), column_not_found);
  }

  CHECK_THROWS(registry.map<Account>("again"), mapping_error);

  struct Unmapped {};
  CHECK_THROWS(registry.meta<Unmapped>(), mapping_error);
  CHECK(registry.find(std::type_index(typeid(Unmapped))) == nullptr);
}
