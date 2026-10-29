#include <cstddef>
#include <string>

#include <uniorm/detail/time.hpp>
#include <uniorm/types.hpp>

#include "libpq/native_types.hpp"

#include "check.hpp"

using namespace uniorm;

void test_libpq_declared_shape() {
  // The width a blessed type declares sits behind the four bytes of header the
  // stored value carries, and an undeclared one answers -1.
  CHECK(libpq::declared_length(-1) == 0);
  CHECK(libpq::declared_length(4) == 0);
  CHECK(libpq::declared_length(34) == 30);

  // An exact number packs precision above scale in the same word.
  int const ten_two = static_cast<int>((10 << 16) | 2) + 4;
  CHECK(libpq::declared_precision(ten_two) == 10);
  CHECK(libpq::declared_scale(ten_two) == 2);
  CHECK(libpq::declared_precision(-1) == 0);
  CHECK(libpq::declared_scale(-1) == 0);

  // A scale of zero is the word's own zero, not an absent one.
  int const eight_zero = static_cast<int>(8 << 16) + 4;
  CHECK(libpq::declared_precision(eight_zero) == 8);
  CHECK(libpq::declared_scale(eight_zero) == 0);
}

void test_libpq_sql_type_from_native() {
  CHECK(libpq::sql_type_from_native(libpq::pg_type::boolean) ==
    sql_type::boolean);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::smallint) ==
    sql_type::smallint);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::integer) ==
    sql_type::integer);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::bigint) ==
    sql_type::bigint);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::real) == sql_type::real);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::double_precision) ==
    sql_type::double_precision);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::numeric) ==
    sql_type::decimal);

  // The server's own identifier text is a string, and an object identifier is
  // a word wider than its integer column.
  CHECK(libpq::sql_type_from_native(libpq::pg_type::name) ==
    sql_type::longvarchar);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::oid) == sql_type::bigint);

  // char(1) arrives as bpchar; the single-byte "char" is a type of its own.
  CHECK(libpq::sql_type_from_native(libpq::pg_type::bpchar) ==
    sql_type::character);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::character) ==
    sql_type::character);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::varchar) ==
    sql_type::varchar);

  // A column of unbounded text, and the shapes the core has no better word for.
  CHECK(libpq::sql_type_from_native(libpq::pg_type::text) ==
    sql_type::longvarchar);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::json) ==
    sql_type::longvarchar);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::jsonb) ==
    sql_type::longvarchar);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::xml) ==
    sql_type::longvarchar);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::bytea) ==
    sql_type::varbinary);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::uuid) == sql_type::guid);

  CHECK(libpq::sql_type_from_native(libpq::pg_type::date) == sql_type::date);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::time) == sql_type::time);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::timetz) == sql_type::time);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::timestamp) ==
    sql_type::timestamp);
  CHECK(libpq::sql_type_from_native(libpq::pg_type::timestamptz) ==
    sql_type::timestamp);

  // An interval has no neutral shape to be read into, and a type number this
  // library has never seen is answered rather than guessed at.
  CHECK(libpq::sql_type_from_native(libpq::pg_type::interval) ==
    sql_type::other);
  CHECK(libpq::sql_type_from_native(4143) == sql_type::other);
}

void test_libpq_display_width() {
  // An exact number reports its precision as the width a value is written in.
  CHECK(libpq::display_width(sql_type::decimal, libpq::pg_type::numeric,
                              static_cast<int>((10 << 16) | 2) + 4) == 10);
  CHECK(libpq::display_width(sql_type::decimal, libpq::pg_type::numeric, -1) ==
    0);

  // A declared character width is the count of characters, and an undeclared
  // one is no width at all: the caller's continuation read gets the value.
  CHECK(libpq::display_width(
    sql_type::varchar, libpq::pg_type::varchar, 34) == 30);
  CHECK(libpq::display_width(
    sql_type::character, libpq::pg_type::bpchar, 5) == 1);
  CHECK(libpq::display_width(
    sql_type::longvarchar, libpq::pg_type::text, -1) == 0);

  // Bytes are counted in bytes, and only for the column that stores them.
  CHECK(libpq::display_width(
    sql_type::varbinary, libpq::pg_type::bytea, 104) == 100);
  CHECK(libpq::display_width(
    sql_type::varbinary, libpq::pg_type::name, -1) == 0);

  // A fixed-width number sizes its own buffer from its type, not from a width.
  CHECK(libpq::display_width(
    sql_type::bigint, libpq::pg_type::bigint, -1) == 0);
  CHECK(libpq::display_width(
    sql_type::timestamp, libpq::pg_type::timestamp, -1) == 0);
}

void test_libpq_text_shape() {
  // Which half of a day and a clock the server's text holds is the column's
  // doing, not the caller's buffer's: a mapped field asks for a whole
  // timestamp even from a column that stores only the clock.
  auto const clock = libpq::parts_of("06:07:08.9", libpq::pg_type::time);
  CHECK(clock.hour == 6 && clock.minute == 7 && clock.second == 8);
  CHECK(clock.fraction_ns == 900000000u);
  // A clock read as a whole timestamp stands on the day the epoch starts, the
  // reading a driver gives for the same conversion.
  CHECK(clock.year == 1970 && clock.month == 1 && clock.day == 1);

  // Asked for a day out of a clock's text, the digits would be taken for a
  // year and a month of zero, which is no date at all.
  auto const day = libpq::parts_of("2024-01-02", libpq::pg_type::date);
  CHECK(day.year == 2024 && day.month == 1 && day.day == 2);
  CHECK(day.hour == 0 && day.minute == 0 && day.second == 0);
  CHECK(day.fraction_ns == 0);

  auto const whole =
    libpq::parts_of("2024-01-02 03:04:05.123456", libpq::pg_type::timestamp);
  CHECK(whole.year == 2024 && whole.month == 1 && whole.day == 2);
  CHECK(whole.hour == 3 && whole.minute == 4 && whole.second == 5);
  CHECK(whole.fraction_ns == 123456000u);
  CHECK(libpq::parts_of("2024-01-02T03:04:05", libpq::pg_type::timestamp)
    .hour == 3);

  // A zone the server appends to a time with time zone is the session's own,
  // already applied, so the wall clock is what went in and the digits behind
  // the sign are not a fraction's.
  auto const zoned = libpq::parts_of("03:04:05.5+08", libpq::pg_type::timetz);
  CHECK(zoned.hour == 3 && zoned.minute == 4 && zoned.second == 5);
  CHECK(zoned.fraction_ns == 500000000u);
  CHECK(zoned.year == 1970 && zoned.month == 1 && zoned.day == 1);

  // The fraction is named in nanoseconds whichever width the server wrote.
  CHECK(libpq::parts_of("03:04:05.1", libpq::pg_type::time).fraction_ns ==
    100000000u);
  CHECK(libpq::parts_of("03:04:05.123456789",
    libpq::pg_type::time).fraction_ns == 123456789u);
}

void test_libpq_text_of_timestamp() {
  // The shape a column's text is read by is the shape it is written by, so a
  // bound value reaches a DATE without a clock and a TIME without a day.
  auto const whole =
    uniorm::detail::make_timestamp(2024, 1, 2, 3, 4, 5, 123456000UL);
  CHECK(libpq::text_of_timestamp(whole, libpq::pg_type::timestamp) ==
    "2024-01-02 03:04:05.123456");
  CHECK(libpq::text_of_timestamp(whole, libpq::pg_type::timestamptz) ==
    "2024-01-02 03:04:05.123456");
  CHECK(libpq::text_of_timestamp(whole, libpq::pg_type::date) == "2024-01-02");
  CHECK(libpq::text_of_timestamp(whole, libpq::pg_type::time) ==
    "03:04:05.123456");
  CHECK(libpq::text_of_timestamp(whole, libpq::pg_type::timetz) ==
    "03:04:05.123456");

  // A type the describe left open keeps the whole shape, which is the one text
  // the server reads against anything it is compared to.
  CHECK(libpq::text_of_timestamp(whole, 0) == "2024-01-02 03:04:05.123456");

  // Each number carries the width the server wrote it in.
  auto const narrow = uniorm::detail::make_timestamp(2005, 1, 2, 9, 8, 7, 0UL);
  CHECK(libpq::text_of_timestamp(narrow, libpq::pg_type::date) == "2005-01-02");
  CHECK(libpq::text_of_timestamp(narrow, libpq::pg_type::timestamp) ==
    "2005-01-02 09:08:07");

  // The fraction is microseconds, written no longer than it needs to be, and a
  // fraction finer than the column keeps is left out rather than rounded.
  auto const half = uniorm::detail::make_timestamp(2024, 1, 2, 3, 4, 5,
    500000000UL);
  CHECK(libpq::text_of_timestamp(half, libpq::pg_type::timestamp) ==
    "2024-01-02 03:04:05.5");
  auto const finest = uniorm::detail::make_timestamp(2024, 1, 2, 3, 4, 5,
    123456789UL);
  CHECK(libpq::text_of_timestamp(finest, libpq::pg_type::timestamp) ==
    "2024-01-02 03:04:05.123456");
  auto const below =
    uniorm::detail::make_timestamp(2024, 1, 2, 3, 4, 5, 999UL);
  CHECK(libpq::text_of_timestamp(below, libpq::pg_type::timestamp) ==
    "2024-01-02 03:04:05");

  // What this backend writes is what it reads: the same value bound to a column
  // of the shape and asked of it again comes to the parts the column names.
  auto const whole_text =
    libpq::text_of_timestamp(whole, libpq::pg_type::timestamp);
  auto const back = libpq::parts_of(whole_text.c_str(),
    libpq::pg_type::timestamp);
  CHECK(back.year == 2024 && back.month == 1 && back.day == 2);
  CHECK(back.hour == 3 && back.minute == 4 && back.second == 5);
  CHECK(back.fraction_ns == 123456000u);

  auto const clock_text = libpq::text_of_timestamp(whole, libpq::pg_type::time);
  auto const clock_back =
    libpq::parts_of(clock_text.c_str(), libpq::pg_type::time);
  CHECK(clock_back.hour == 3 && clock_back.minute == 4);
  CHECK(clock_back.second == 5 && clock_back.fraction_ns == 123456000u);
  CHECK(clock_back.year == 1970 && clock_back.month == 1);
  CHECK(clock_back.day == 1);

  auto const day_text = libpq::text_of_timestamp(whole, libpq::pg_type::date);
  auto const day_back = libpq::parts_of(day_text.c_str(), libpq::pg_type::date);
  CHECK(day_back.year == 2024 && day_back.month == 1 && day_back.day == 2);
  CHECK(day_back.hour == 0 && day_back.fraction_ns == 0);
}
