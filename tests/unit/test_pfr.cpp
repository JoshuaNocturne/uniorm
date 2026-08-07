#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <uniorm/detail/pfr.hpp>

#include "check.hpp"

using namespace uniorm;

namespace {

struct empty {};

struct one {
  int a;
};

struct mixed {
  std::int64_t id;
  std::string name;
  std::optional<int> age;
  double score;
  std::vector<std::byte> blob;
};

struct defaulted {
  int a = 1;
  int b = 2;
  int c = 3;
};

struct not_aggregate {
  not_aggregate(int x) : v(x) {}
  int v;
};

}  // namespace

void test_pfr() {
  static_assert(detail::field_count<empty>() == 0);
  static_assert(detail::field_count<one>() == 1);
  static_assert(detail::field_count<mixed>() == 5);
  static_assert(detail::field_count<defaulted>() == 3);

  static_assert(detail::aggregate_projection<mixed>);
  static_assert(detail::aggregate_projection<empty>);
  static_assert(!detail::aggregate_projection<not_aggregate>);
  static_assert(!detail::aggregate_projection<std::string>);

  // tie_aggregate returns references to the original fields
  one o{ 42 };
  auto t1 = detail::tie_aggregate(o);
  std::get<0>(t1) = 7;
  CHECK(o.a == 7);

  // for_each_field visits all fields in order; count types visited
  mixed m{ 5, "alice", std::nullopt, 3.5, {} };
  int visited = 0;
  detail::for_each_field(m, [&](auto& field) {
    ++visited;
    using F = std::remove_cvref_t<decltype(field)>;
    if constexpr (std::is_same_v<F, std::int64_t>) {
      CHECK(field == 5);
    } else if constexpr (std::is_same_v<F, std::string>) {
      CHECK(field == "alice");
    } else if constexpr (std::is_same_v<F, std::optional<int>>) {
      CHECK(!field.has_value());
    }
  });
  CHECK(visited == 5);

  // fields with default member initializers still count correctly
  defaulted d;
  int sum = 0;
  detail::for_each_field(d, [&](auto& f) { sum += f; });
  CHECK(sum == 6);
}
