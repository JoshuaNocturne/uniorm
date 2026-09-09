#include "check.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <uniorm/decimal.hpp>
#include <uniorm/row.hpp>

namespace {

uniorm::decimal_t dec(std::string_view text) {
  return uniorm::decimal_t::from_literal(text);
}

std::string digits(int count, char digit) {
  return std::string(static_cast<std::size_t>(count), digit);
}

void test_literal_round_trip() {
  CHECK(dec("0").to_literal() == "0");
  CHECK(dec("0.1000").to_literal() == "0.1000");  // scale survives
  CHECK(dec("001.5").to_literal() == "1.5");      // integer zeros do not
  CHECK(dec("+12").to_literal() == "12");
  CHECK(dec(".5").to_literal() == "0.5");
  CHECK(dec("-0.000").to_literal() == "0.000");  // no negative zero
  CHECK(dec("-1020.30").to_literal() == "-1020.30");

  std::string const widest = digits(65, '9');  // a server's widest DECIMAL
  CHECK(dec(widest).to_literal() == widest);
  CHECK(dec("0." + digits(60, '1') + '7').to_literal() ==
        "0." + digits(60, '1') + '7');

  CHECK(dec("0.1000").scale() == 4);
  CHECK(dec("7").scale() == 0);
  CHECK(dec("-7").negative());
  CHECK(!dec("7").negative());

  // Default holds the same value as the literal for zero, so a struct member
  // never has to be assigned before it compares.
  CHECK(uniorm::decimal_t{} == dec("0.0000"));
}

void test_rejects_what_is_not_an_exact_literal() {
  CHECK_THROWS(dec(""), uniorm::type_mismatch);
  CHECK_THROWS(dec("-"), uniorm::type_mismatch);
  CHECK_THROWS(dec("."), uniorm::type_mismatch);
  CHECK_THROWS(dec("1.2.3"), uniorm::type_mismatch);
  CHECK_THROWS(dec("1e5"), uniorm::type_mismatch);
  CHECK_THROWS(dec("1 000"), uniorm::type_mismatch);
  CHECK_THROWS(dec("abc"), uniorm::type_mismatch);
  CHECK_THROWS(dec(digits(79, '1')), uniorm::type_mismatch);
  CHECK(dec(digits(78, '1')).to_literal() == digits(78, '1'));
}

void test_orders_across_scales() {
  CHECK(dec("1.5") == dec("1.50"));
  CHECK(dec("1.50") == dec("1.500"));
  CHECK(dec("0") == dec("0.000"));
  CHECK(dec("0.000") == dec("-0"));
  CHECK(dec("1.5") < dec("1.51"));
  CHECK(dec("1.5") > dec("1.4999"));
  CHECK(dec("100") > dec("99.99"));
  CHECK(dec("0.1") > dec("0.099999999999999999999"));
  CHECK(dec("-2") < dec("-1.9999"));
  CHECK(dec("-2") < dec("0"));
  CHECK(dec(digits(65, '9')) > dec(digits(64, '9') + "1"));
  CHECK((dec("1") <=> dec("1.0")) == std::strong_ordering::equal);
  CHECK((dec("2") <=> dec("1.5")) == std::strong_ordering::greater);
}

void test_converts_out() {
  CHECK(dec("7").to_int64() == 7);
  CHECK(dec("7.00").to_int64() == 7);
  CHECK(dec("-7.000").to_int64() == -7);
  CHECK(dec("9223372036854775807").to_int64() ==
        std::numeric_limits<std::int64_t>::max());
  CHECK(dec("-9223372036854775808").to_int64() ==
        std::numeric_limits<std::int64_t>::min());
  CHECK_THROWS(dec("7.5").to_int64(), uniorm::type_mismatch);
  CHECK_THROWS(dec("9223372036854775808").to_int64(), uniorm::type_mismatch);
  CHECK_THROWS(dec("-9223372036854775809").to_int64(), uniorm::type_mismatch);
  // Wider than any integer: the digits are exact, the conversion is not.
  CHECK_THROWS(dec(digits(40, '9')).to_int64(), uniorm::type_mismatch);

  CHECK(dec("0.1000").to_double() == 0.1);
  CHECK(dec("-1020.30").to_double() == -1020.3);
  CHECK(dec("0").to_double() == 0.0);
  CHECK(dec("-0.5").to_double() == -0.5);
}

void test_reaches_a_row_value() {
  uniorm::sql_value exact{ std::string("0.1000") };
  CHECK(uniorm::value_cast<uniorm::decimal_t>(exact) == dec("0.1"));
  CHECK(uniorm::value_cast<uniorm::decimal_t>(exact).to_literal() == "0.1000");
  CHECK(uniorm::value_cast<std::string>(exact) == "0.1000");
  // An integer already in the process has no literal to parse.
  uniorm::sql_value integral{ std::int64_t{ 5 } };
  CHECK_THROWS(
    uniorm::value_cast<uniorm::decimal_t>(integral), uniorm::type_mismatch);
  uniorm::sql_value null;
  CHECK(uniorm::value_cast<std::optional<uniorm::decimal_t>>(null) ==
        std::nullopt);
  CHECK(uniorm::value_cast<std::optional<uniorm::decimal_t>>(exact).value() ==
        dec("0.1000"));

  // The representation is the literal, so a write path encodes to it -- into
  // the slot it is handed, which it replaces instead of extending.
  std::string slot(64, '9');
  uniorm::converter<uniorm::decimal_t>::to_db(dec("-3.50"), slot);
  CHECK(slot == "-3.50");
  dec("0.1000").to_literal_into(slot);
  CHECK(slot == "0.1000");
  CHECK(uniorm::converter<uniorm::decimal_t>::from_db(std::move(slot)) ==
        dec("0.1"));
}

}  // namespace

void test_decimal() {
  test_literal_round_trip();
  test_rejects_what_is_not_an_exact_literal();
  test_orders_across_scales();
  test_converts_out();
  test_reaches_a_row_value();
}
