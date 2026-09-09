#include "uniorm/decimal.hpp"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <limits>

#include "uniorm/error.hpp"

namespace uniorm {

decimal_t decimal_t::from_literal(std::string_view text) {
  decimal_t out;
  out.len_ = 0;
  out.scale_ = 0;
  bool negative = false;
  bool after_point = false;
  int integer_digits = 0;
  std::size_t i = 0;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
    negative = text[i] == '-';
    ++i;
  }
  for (; i < text.size(); ++i) {
    char const c = text[i];
    if (c == '.') {
      if (after_point) {
        throw type_mismatch("decimal_t literal has two radix points");
      }
      after_point = true;
      continue;
    }
    // Rejects an exponent, a thousands separator and any space: a literal that
    // needs evaluating is not the exact numeric this type stands for.
    if (c < '0' || c > '9') {
      throw type_mismatch("decimal_t literal is not a run of digits");
    }
    if (out.len_ == max_digits) {
      throw type_mismatch("decimal_t literal needs more digits than it holds");
    }
    out.digits_[out.len_] = static_cast<std::uint8_t>(c - '0');
    ++out.len_;
    if (after_point) {
      ++out.scale_;
    } else {
      ++integer_digits;
    }
  }
  if (integer_digits == 0) {
    if (out.len_ == 0) {
      throw type_mismatch("decimal_t literal holds no digits");
    }
    // ".5" means 0.5: the integer part keeps a digit to count the scale from.
    if (out.len_ == max_digits) {
      throw type_mismatch("decimal_t literal needs more digits than it holds");
    }
    for (int k = out.len_; k > 0; --k) {
      out.digits_[k] = out.digits_[k - 1];
    }
    out.digits_[0] = 0;
    ++out.len_;
    integer_digits = 1;
  }
  int first = 0;
  while (first + 1 < integer_digits && out.digits_[first] == 0) {
    ++first;
  }
  for (int k = first; k < out.len_; ++k) {
    out.digits_[k - first] = out.digits_[k];
  }
  out.len_ = static_cast<std::uint8_t>(out.len_ - first);
  bool all_zero = true;
  for (int k = 0; k < out.len_; ++k) {
    all_zero = all_zero && out.digits_[k] == 0;
  }
  // A negated zero would order itself below every positive scale of zero.
  out.negative_ = negative && !all_zero;
  return out;
}

int decimal_t::integer_len() const noexcept {
  return static_cast<int>(len_) - scale_;
}

int decimal_t::integer_first() const noexcept {
  int const stop = integer_len();
  int first = 0;
  for (; first + 1 < stop && digits_[first] == 0; ++first) {
  }
  return first;
}

int decimal_t::scale() const noexcept {
  return scale_;
}

bool decimal_t::negative() const noexcept {
  return negative_;
}

void decimal_t::to_literal_into(std::string& out) const {
  out.clear();
  out.reserve(static_cast<std::size_t>(
    len_ + (scale_ > 0 ? 1 : 0) + (negative_ ? 1 : 0)));
  if (negative_) {
    out += '-';
  }
  int const point = integer_len();
  for (int i = 0; i < point; ++i) {
    out += static_cast<char>('0' + digits_[i]);
  }
  if (scale_ > 0) {
    out += '.';
    for (int i = point; i < len_; ++i) {
      out += static_cast<char>('0' + digits_[i]);
    }
  }
}

std::string decimal_t::to_literal() const {
  std::string out;
  to_literal_into(out);
  return out;
}

double decimal_t::to_double() const {
  long double accumulated = 0.0L;
  for (int i = 0; i < len_; ++i) {
    accumulated = accumulated * 10.0L + digits_[i];
  }
  for (int s = 0; s < scale_; ++s) {
    accumulated /= 10.0L;
  }
  return negative_ ? -static_cast<double>(accumulated)
                   : static_cast<double>(accumulated);
}

std::int64_t decimal_t::to_int64() const {
  int const point = integer_len();
  for (int i = point; i < len_; ++i) {
    if (digits_[i] != 0) {
      throw type_mismatch("decimal_t value has a fraction to drop");
    }
  }
  std::uint64_t magnitude = 0;
  for (int i = 0; i < point; ++i) {
    std::uint64_t const digit = digits_[i];
    if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      throw type_mismatch("decimal_t value is out of int64 range");
    }
    magnitude = magnitude * 10 + digit;
  }
  std::uint64_t const limit = negative_
    ? std::uint64_t{ 1 } << 63
    : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (magnitude > limit) {
    throw type_mismatch("decimal_t value is out of int64 range");
  }
  return negative_ ? static_cast<std::int64_t>(~magnitude + 1)
                   : static_cast<std::int64_t>(magnitude);
}

int decimal_t::compare_magnitude(decimal_t const& o) const noexcept {
  int const a_first = integer_first(), b_first = o.integer_first();
  int const a_len = integer_len() - a_first;
  int const b_len = o.integer_len() - b_first;
  if (a_len != b_len) {
    return a_len < b_len ? -1 : 1;
  }
  for (int i = 0; i < a_len; ++i) {
    if (digits_[a_first + i] != o.digits_[b_first + i]) {
      return digits_[a_first + i] < o.digits_[b_first + i] ? -1 : 1;
    }
  }
  int const a_start = integer_len(), b_start = o.integer_len();
  for (int i = 0; i < scale_ || i < o.scale_; ++i) {
    std::uint8_t const a = i < scale_ ? digits_[a_start + i] : 0;
    std::uint8_t const b = i < o.scale_ ? o.digits_[b_start + i] : 0;
    if (a != b) {
      return a < b ? -1 : 1;
    }
  }
  return 0;
}

std::strong_ordering operator<=>(
  decimal_t const& lhs, decimal_t const& rhs) noexcept {
  // A zero is never negative, so differing signs always order the values.
  if (lhs.negative_ != rhs.negative_) {
    return lhs.negative_ ? std::strong_ordering::less
                         : std::strong_ordering::greater;
  }
  int const magnitude = lhs.compare_magnitude(rhs);
  if (magnitude == 0) {
    return std::strong_ordering::equal;
  }
  return (magnitude < 0) != lhs.negative_ ? std::strong_ordering::less
                                          : std::strong_ordering::greater;
}

bool operator==(decimal_t const& lhs, decimal_t const& rhs) noexcept {
  return (lhs <=> rhs) == std::strong_ordering::equal;
}

void converter<decimal_t>::to_db(decimal_t const& value, std::string& out) {
  value.to_literal_into(out);
}

decimal_t converter<decimal_t>::from_db(std::string&& text) {
  return decimal_t::from_literal(text);
}

}  // namespace uniorm
