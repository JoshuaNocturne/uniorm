#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include <uniorm/converter.hpp>
#include <uniorm/export.hpp>

namespace uniorm {

// Exact fixed-point value: unscaled digits with the count of fraction digits
// under the radix point. Its 78 digits clear the 65 a MariaDB DECIMAL can name
// without a byte of its own allocation, so a wider text has to be a
// std::string, which carries it unchanged.
class UNIORM_API decimal_t {
public:
  static constexpr int max_digits = 78;

  constexpr decimal_t() noexcept = default;

  // [-]digits[.digits], the shape every driver returns for an exact numeric.
  // Throws type_mismatch on an exponent, a separator, or a literal with more
  // significant digits than max_digits.
  static decimal_t from_literal(std::string_view text);

  // The value as it was read, minus the leading zeros of the integer part:
  // 0.1000 keeps its four fraction digits, 001.5 does not keep its two
  // integer ones.
  std::string to_literal() const;

  // Same text into a caller-owned slot, which it clears: so a batch write
  // reuses that capacity instead of allocating per row.
  void to_literal_into(std::string& out) const;

  // Fraction digits as the literal spelled them; never normalized away, so an
  // order comparison and this getter can disagree about 1.50.
  int scale() const noexcept;
  bool negative() const noexcept;

  // Rounds the way a double conversion must; the digits hold the exact value.
  double to_double() const;
  // Throws type_mismatch when a fraction digit is nonzero or the integer part
  // does not fit.
  std::int64_t to_int64() const;

  friend UNIORM_API std::strong_ordering operator<=>(
    decimal_t const& lhs, decimal_t const& rhs) noexcept;
  friend UNIORM_API bool operator==(
    decimal_t const& lhs, decimal_t const& rhs) noexcept;

private:
  // Value = digits[0, len_) read as an integer, times 10^-scale_. The integer
  // part keeps at least one digit, which is what makes scale_ <= len_.
  int integer_len() const noexcept;

  // Last digit of a zero integer part, so a zero still compares by one digit.
  int integer_first() const noexcept;

  // Ignores the sign and the scale width, so 1.5 and 1.50 come out equal.
  int compare_magnitude(decimal_t const& o) const noexcept;

  std::array<std::uint8_t, max_digits> digits_{};
  std::uint8_t len_ = 1;
  std::int16_t scale_ = 0;
  bool negative_ = false;
};

// One representation, the driver's own literal: a decimal_t field binds exactly
// as the std::string a DECIMAL column defaults to, so the lossless read stays
// one route and this only names the value it decodes to. Exported because the
// templates that reach it -- value_cast, the projection bindings, the entity
// registry -- are instantiated in the caller's translation unit.
template <>
struct UNIORM_API converter<decimal_t> {
  using db_type = std::string;

  static void to_db(decimal_t const& value, std::string& out);
  static decimal_t from_db(std::string&& text);
};

}  // namespace uniorm
