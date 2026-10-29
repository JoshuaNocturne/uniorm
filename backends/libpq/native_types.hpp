#pragma once

// Private header: PostgreSQL's type numbers translated to the neutral ones,
// the two readings a type modifier carries, the server's own text of a date or
// a clock read into the parts the core stages them in, and those parts written
// back the way the column asked for them. Not installed.
// libpq-fe.h names none of these numbers, so they are spelled as pg_type holds
// them and this is the one place in the tree where they appear.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

#include <uniorm/backend/backend.hpp>
#include <uniorm/detail/time.hpp>
#include <uniorm/types.hpp>

namespace uniorm::libpq {

namespace pg_type {

inline constexpr unsigned boolean = 16;
inline constexpr unsigned bytea = 17;
inline constexpr unsigned character = 18;
inline constexpr unsigned name = 19;
inline constexpr unsigned bigint = 20;
inline constexpr unsigned smallint = 21;
inline constexpr unsigned integer = 23;
inline constexpr unsigned text = 25;
inline constexpr unsigned oid = 26;
inline constexpr unsigned json = 114;
inline constexpr unsigned xml = 142;
inline constexpr unsigned real = 700;
inline constexpr unsigned double_precision = 701;
inline constexpr unsigned bpchar = 1042;
inline constexpr unsigned varchar = 1043;
inline constexpr unsigned date = 1082;
inline constexpr unsigned time = 1083;
inline constexpr unsigned timestamp = 1114;
inline constexpr unsigned timestamptz = 1184;
inline constexpr unsigned interval = 1186;
inline constexpr unsigned timetz = 1266;
inline constexpr unsigned numeric = 1700;
inline constexpr unsigned uuid = 2950;
inline constexpr unsigned jsonb = 3802;

}  // namespace pg_type

// The width a blessed variable-length type declares is its type modifier minus
// the four bytes of varlena header it carries: -1 for anything undeclared,
// which reads back as no width at all.
inline std::size_t declared_length(int modifier) {
  return modifier < 4 ? 0 : static_cast<std::size_t>(modifier - 4);
}

// numeric packs its declaration into the same word, precision above scale.
inline std::int32_t declared_precision(int modifier) {
  return modifier < 4 ? 0 : static_cast<std::int32_t>((modifier - 4) >> 16);
}

inline std::int32_t declared_scale(int modifier) {
  return modifier < 4 ? 0 : static_cast<std::int32_t>((modifier - 4) & 0xffff);
}

// A column's neutral type. A "char" is the server's own single-byte type, not
// the char(1) a table declares -- that one arrives as bpchar. An interval has
// no neutral shape to be read into.
inline sql_type sql_type_from_native(unsigned type) {
  switch (type) {
  case pg_type::boolean:
    return sql_type::boolean;
  case pg_type::character:
    return sql_type::character;
  case pg_type::smallint:
    return sql_type::smallint;
  case pg_type::integer:
    return sql_type::integer;
  case pg_type::bigint:
    return sql_type::bigint;
  // An object identifier runs the width of an unsigned 32-bit word, past what
  // the server's own integer column holds.
  case pg_type::oid:
    return sql_type::bigint;
  case pg_type::real:
    return sql_type::real;
  case pg_type::double_precision:
    return sql_type::double_precision;
  case pg_type::numeric:
    return sql_type::decimal;
  case pg_type::bpchar:
    return sql_type::character;
  case pg_type::varchar:
    return sql_type::varchar;
  // name is the server's fixed-width text of its own identifiers.
  case pg_type::name:
  case pg_type::text:
  case pg_type::xml:
  case pg_type::json:
  case pg_type::jsonb:
    return sql_type::longvarchar;
  case pg_type::bytea:
    return sql_type::varbinary;
  case pg_type::date:
    return sql_type::date;
  case pg_type::time:
  case pg_type::timetz:
    return sql_type::time;
  case pg_type::timestamp:
  case pg_type::timestamptz:
    return sql_type::timestamp;
  case pg_type::uuid:
    return sql_type::guid;
  default:
    return sql_type::other;
  }
}

// The width the core sizes a value buffer from: characters for a text column,
// bytes for a binary one, the precision for an exact number. A type that
// declares nothing answers zero, and the caller's continuation read gets the
// value whole.
inline std::size_t display_width(
  sql_type type, unsigned native_type, int modifier) {
  switch (type) {
  case sql_type::decimal:
    return static_cast<std::size_t>(declared_precision(modifier));
  case sql_type::character:
  case sql_type::varchar:
    return declared_length(modifier);
  case sql_type::varbinary:
    return native_type == pg_type::bytea ? declared_length(modifier) : 0;
  default:
    return 0;
  }
}

// Up to the given number of digits from the run the scanner stands on, moving
// it past whatever it read.
inline std::uint64_t take_digits(char const*& scan, std::size_t wanted) {
  std::uint64_t value = 0;
  for (std::size_t read = 0; read < wanted; ++read) {
    auto const byte = static_cast<unsigned char>(*scan);
    if (std::isdigit(byte) == 0) {
      break;
    }
    value = value * 10 + static_cast<std::uint64_t>(byte - '0');
    ++scan;
  }
  return value;
}

// The server's own text of a date and a time: "2024-01-02 03:04:05.123456",
// or one half of it for the narrower columns. A zone the server appends to a
// time with time zone is the session's own, already applied, so the wall-clock
// reading is the value that went in.
inline backend::timestamp_parts parts_of(
  char const* text, bool wants_date, bool wants_time) {
  backend::timestamp_parts out{};
  char const* scan = text;
  if (wants_date) {
    out.year = static_cast<std::int16_t>(take_digits(scan, 4));
    if (*scan == '-') {
      ++scan;
    }
    out.month = static_cast<std::uint16_t>(take_digits(scan, 2));
    if (*scan == '-') {
      ++scan;
    }
    out.day = static_cast<std::uint16_t>(take_digits(scan, 2));
    if (wants_time && (*scan == ' ' || *scan == 'T')) {
      ++scan;
    }
  }
  if (wants_time) {
    out.hour = static_cast<std::uint16_t>(take_digits(scan, 2));
    if (*scan == ':') {
      ++scan;
    }
    out.minute = static_cast<std::uint16_t>(take_digits(scan, 2));
    if (*scan == ':') {
      ++scan;
    }
    out.second = static_cast<std::uint16_t>(take_digits(scan, 2));
    if (*scan == '.') {
      ++scan;
      std::uint64_t fraction = 0;
      std::size_t digits = 0;
      for (; digits < 9; ++digits) {
        auto const byte = static_cast<unsigned char>(*scan);
        if (std::isdigit(byte) == 0) {
          break;
        }
        fraction = fraction * 10 + static_cast<std::uint64_t>(byte - '0');
        ++scan;
      }
      for (; digits < 9; ++digits) {
        fraction *= 10;
      }
      out.fraction_ns = static_cast<std::uint32_t>(fraction);
    }
  }
  return out;
}

// What the server's text of one of these columns holds. The caller's buffer
// does not decide it: a mapped field asks for a whole timestamp even from a
// column that stores only one half of one.
struct text_shape {
  bool has_day;
  bool has_clock;
};

inline text_shape shape_of(unsigned native) {
  if (native == pg_type::time || native == pg_type::timetz) {
    return { false, true };
  }
  if (native == pg_type::date) {
    return { true, false };
  }
  return { true, true };
}

// The parts a column of this type names, with the half it does not store read
// as the shape's empty half means: a clock without a day stands on the day the
// epoch starts, which is the reading every driver gives for a TIME converted to
// a timestamp.
inline backend::timestamp_parts parts_of(char const* text, unsigned native) {
  text_shape const shape = shape_of(native);
  auto out = parts_of(text, shape.has_day, shape.has_clock);
  if (!shape.has_day) {
    out.year = 1970;
    out.month = 1;
    out.day = 1;
  }
  return out;
}

// A number written to the width the server pads it to.
inline std::string padded(std::uint64_t value, std::size_t width) {
  std::string out = std::to_string(value);
  while (out.size() < width) {
    out.insert(out.begin(), '0');
  }
  return out;
}

// The parts back into the words of the column they are bound to: the shape
// that decides how a text is taken apart is the one that decides how it is
// written, so a date carries no clock and a clock no day. A type the describe
// left open keeps the whole shape, which every one of these takes.
inline std::string text_of_timestamp(
  timestamp const& value, unsigned native) {
  auto const parts = uniorm::detail::break_timestamp(value);
  text_shape const shape = shape_of(native);
  std::string clock;
  if (shape.has_clock) {
    clock = padded(parts.hour, 2) + ':' + padded(parts.minute, 2) + ':' +
      padded(parts.second, 2);
    // The server keeps microseconds, and a fraction it cannot hold is left out
    // rather than sent as a rounding of it.
    std::uint64_t const micros = parts.fraction_ns / 1000UL;
    if (micros != 0) {
      std::string fraction = padded(micros, 6);
      while (fraction.back() == '0') {
        fraction.pop_back();
      }
      clock += '.';
      clock += fraction;
    }
  }
  if (!shape.has_day) {
    return clock;
  }
  std::string const date =
    padded(static_cast<std::uint64_t>(parts.year), 4) + '-' +
    padded(parts.month, 2) + '-' + padded(parts.day, 2);
  return shape.has_clock ? date + ' ' + clock : date;
}

}  // namespace uniorm::libpq
