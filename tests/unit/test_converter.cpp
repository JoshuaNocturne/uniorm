// Converter extension point: the value layer encodes a domain type through
// to_db and decodes it through from_db. These cases pin the ordering that
// makes the feature usable at all -- an adapter must win over the implicit
// enum-to-integer and implicit-string-conversion arms, which bind a domain
// type to something other than what its converter says.

#include <cstdint>
#include <string>
#include <vector>

#include <uniorm/builder/expression.hpp>
#include <uniorm/converter.hpp>
#include <uniorm/params.hpp>

#include "check.hpp"

using namespace uniorm;

namespace {

enum class status { fresh, paid, shipped };

// No converter: kept to show that the pre-existing enum arm is untouched.
enum class plain_enum { three = 3 };

struct money {
  std::int64_t units;
  std::int64_t micros;
};

// Has an implicit conversion to std::string, which is exactly the arm that
// would otherwise claim it.
struct tagged {
  int code;

  operator std::string() const {
    return "via-implicit";
  }
};

struct record {
  std::int64_t id;
  status state;
};

// Declared with only sql and to_db: detection must reject it rather than fail
// hard, so a forgotten from_db surfaces as "unsupported type" downstream.
struct half_wired {
  int value;
};

}  // namespace

namespace uniorm {

template <>
struct converter<status> {
  using sql = std::string;

  static void to_db(status const& s, std::string& out) {
    out = s == status::paid   ? "paid"
          : s == status::shipped ? "shipped"
      : "fresh";
  }

  static status from_db(std::string const& v) {
    return v == "paid" ? status::paid
      : v == "shipped" ? status::shipped
      : status::fresh;
  }
};

template <>
struct converter<money> {
  using sql = std::string;

  static void to_db(money const& m, std::string& out) {
    out = std::to_string(m.units) + "." + std::to_string(m.micros);
    out.resize(16, '0');  // past the short-string buffer on purpose
  }

  static money from_db(std::string const&) {
    return {};
  }
};

template <>
struct converter<tagged> {
  using sql = std::int16_t;

  static void to_db(tagged const& t, std::int16_t& out) {
    out = static_cast<std::int16_t>(t.code);
  }

  static tagged from_db(std::int16_t const&) {
    return {};
  }
};

template <>
struct converter<half_wired> {
  using sql = std::string;

  static void to_db(half_wired const&, std::string& out) {
    out = "incomplete";
  }
};

}  // namespace uniorm

void test_converter() {
  CHECK(has_converter<status>);
  CHECK(has_converter<money>);
  CHECK(!has_converter<int>);
  CHECK(!has_converter<std::string>);
  CHECK(!has_converter<plain_enum>);
  CHECK(!has_converter<half_wired>);

  // The representation the converter names, not the integer the enum would
  // otherwise decay to.
  params p(status::paid, money{ 1234567, 890 });
  CHECK(std::get<std::string>(p.at(0)) == "paid");
  CHECK(std::get<std::string>(p.at(1)).size() == 16);
  CHECK(std::get<std::string>(p.at(1)).rfind("1234567.", 0) == 0);

  // An adapter beats an implicit conversion to std::string.
  params q(tagged{ 7 });
  CHECK(std::holds_alternative<std::int16_t>(q.at(0)));
  CHECK(std::get<std::int16_t>(q.at(0)) == 7);

  // Unconverted enums keep their existing path.
  params r(plain_enum::three);
  CHECK(std::get<std::int32_t>(r.at(0)) == 3);

  // Query predicates share the same encoding.
  std::vector<sql_value> bound;
  auto sql = eq(&record::state, status::shipped)
               .to_sql([](member_key const&) { return std::string("state"); },
                 bound);
  CHECK(sql == "state = ?");
  CHECK(bound.size() == 1);
  CHECK(std::get<std::string>(bound[0]) == "shipped");
}
