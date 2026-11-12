#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <uniorm/value.hpp>

namespace uniorm::detail {

// Chrono-only helpers shared between parameter binding and result materializing
// (the ODBC SQL_*_STRUCT conversions live in .cpp files).

inline timestamp make_timestamp(int year, unsigned month, unsigned day,
  unsigned hour, unsigned minute, unsigned second, unsigned long fraction_ns) {
  std::chrono::year y{ year };
  std::chrono::month m{ static_cast<unsigned>(month) };
  std::chrono::day d{ static_cast<unsigned>(day) };
  std::chrono::sys_days days = y / m / d;
  auto duration = days.time_since_epoch() + std::chrono::hours(hour) +
                  std::chrono::minutes(minute) + std::chrono::seconds(second) +
                  std::chrono::nanoseconds(fraction_ns);
  return timestamp(
    std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));
}

struct timestamp_parts {
  int year;
  unsigned month;
  unsigned day;
  unsigned hour;
  unsigned minute;
  unsigned second;
  unsigned long fraction_ns;
};

inline timestamp_parts break_timestamp(timestamp tp) {
  auto days = std::chrono::floor<std::chrono::days>(tp);
  std::chrono::year_month_day ymd{ days };
  auto tod = std::chrono::hh_mm_ss<std::chrono::nanoseconds>{
    std::chrono::duration_cast<std::chrono::nanoseconds>(tp - days)
  };
  return { static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
    static_cast<unsigned>(ymd.day()),
    static_cast<unsigned>(tod.hours().count()),
    static_cast<unsigned>(tod.minutes().count()),
    static_cast<unsigned>(tod.seconds().count()),
    static_cast<unsigned long>(tod.subseconds().count()) };
}

// A number written to the width a server pads it to.
inline std::string pad_digits(std::uint64_t value, std::size_t width) {
  std::string out = std::to_string(value);
  while (out.size() < width) {
    out.insert(out.begin(), '0');
  }
  return out;
}

// The parts back into the words of the column they are bound to: the shape
// that decides how a text is taken apart is the one that decides how it is
// written, so a date carries no clock and a clock no day.
inline std::string text_of_timestamp(
  timestamp const& value, bool has_day, bool has_clock) {
  timestamp_parts const parts = break_timestamp(value);
  std::string clock;
  if (has_clock) {
    clock = pad_digits(parts.hour, 2) + ':' + pad_digits(parts.minute, 2) +
            ':' + pad_digits(parts.second, 2);
    // The server keeps microseconds, and a fraction it cannot hold is left out
    // rather than sent as a rounding of it.
    std::uint64_t const micros = parts.fraction_ns / 1000UL;
    if (micros != 0) {
      std::string fraction = pad_digits(micros, 6);
      while (fraction.back() == '0') {
        fraction.pop_back();
      }
      clock += '.';
      clock += fraction;
    }
  }
  if (!has_day) {
    return clock;
  }
  std::string const date =
    pad_digits(static_cast<std::uint64_t>(parts.year), 4) + '-' +
    pad_digits(parts.month, 2) + '-' + pad_digits(parts.day, 2);
  return has_clock ? date + ' ' + clock : date;
}

}  // namespace uniorm::detail
