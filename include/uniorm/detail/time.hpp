#pragma once

#include <chrono>

#include "../value.hpp"

namespace uniorm::detail {

// Chrono-only helpers shared between parameter binding and result materializing
// (the ODBC SQL_*_STRUCT conversions live in .cpp files).

inline timestamp make_timestamp(int year, unsigned month, unsigned day,
  unsigned hour, unsigned minute, unsigned second, unsigned long fraction_ns) {
  std::chrono::year y{ year };
  std::chrono::month m{ static_cast<unsigned>(month) };
  std::chrono::day d{ static_cast<unsigned>(day) };
  std::chrono::sys_days days = y / m / d;
  return timestamp{ days.time_since_epoch() } + std::chrono::hours(hour) +
         std::chrono::minutes(minute) + std::chrono::seconds(second) +
         std::chrono::nanoseconds(fraction_ns);
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

}  // namespace uniorm::detail
