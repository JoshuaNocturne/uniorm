#pragma once

// pfr-lite: minimal aggregate reflection for row projection binding.
// Technique: a universal-convertible placeholder type probes aggregate
// initialization arities via requires-expressions (field count), and a
// macro-generated if-constexpr chain decomposes the aggregate through
// structured bindings. Limitations: aggregates only, no base classes,
// no array members, at most 64 fields.

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace uniorm::detail {

inline constexpr std::size_t max_aggregate_fields = 64;

struct ubiq {
  template <class T>
  constexpr operator T&() const noexcept {
    // Only used in unevaluated contexts (requires expressions); never called.
    __builtin_unreachable();
  }
};

template <class T, std::size_t... I>
constexpr bool can_init_with(std::index_sequence<I...>) {
  return requires { T{ ((void)I, ubiq{})... }; };
}

template <class T, std::size_t N>
constexpr std::size_t detect_field_count() {
  if constexpr (can_init_with<T>(std::make_index_sequence<N>{})) {
    return N;
  } else if constexpr (N == 0) {
    return 0;
  } else {
    return detect_field_count<T, N - 1>();
  }
}

template <class T>
constexpr std::size_t field_count() {
  using U = std::remove_cvref_t<T>;
  static_assert(
    !can_init_with<U>(std::make_index_sequence<max_aggregate_fields + 1>{}),
    "aggregate has more than uniorm::detail::max_aggregate_fields fields");
  return detect_field_count<U, max_aggregate_fields>();
}

template <class T>
concept aggregate_projection =
  std::is_aggregate_v<std::remove_cvref_t<T>> &&
  std::is_default_constructible_v<std::remove_cvref_t<T>> &&
  (field_count<std::remove_cvref_t<T>>() <= max_aggregate_fields);

#define UNIORM_PFR_F1 f1
#define UNIORM_PFR_F2 UNIORM_PFR_F1, f2
#define UNIORM_PFR_F3 UNIORM_PFR_F2, f3
#define UNIORM_PFR_F4 UNIORM_PFR_F3, f4
#define UNIORM_PFR_F5 UNIORM_PFR_F4, f5
#define UNIORM_PFR_F6 UNIORM_PFR_F5, f6
#define UNIORM_PFR_F7 UNIORM_PFR_F6, f7
#define UNIORM_PFR_F8 UNIORM_PFR_F7, f8
#define UNIORM_PFR_F9 UNIORM_PFR_F8, f9
#define UNIORM_PFR_F10 UNIORM_PFR_F9, f10
#define UNIORM_PFR_F11 UNIORM_PFR_F10, f11
#define UNIORM_PFR_F12 UNIORM_PFR_F11, f12
#define UNIORM_PFR_F13 UNIORM_PFR_F12, f13
#define UNIORM_PFR_F14 UNIORM_PFR_F13, f14
#define UNIORM_PFR_F15 UNIORM_PFR_F14, f15
#define UNIORM_PFR_F16 UNIORM_PFR_F15, f16
#define UNIORM_PFR_F17 UNIORM_PFR_F16, f17
#define UNIORM_PFR_F18 UNIORM_PFR_F17, f18
#define UNIORM_PFR_F19 UNIORM_PFR_F18, f19
#define UNIORM_PFR_F20 UNIORM_PFR_F19, f20
#define UNIORM_PFR_F21 UNIORM_PFR_F20, f21
#define UNIORM_PFR_F22 UNIORM_PFR_F21, f22
#define UNIORM_PFR_F23 UNIORM_PFR_F22, f23
#define UNIORM_PFR_F24 UNIORM_PFR_F23, f24
#define UNIORM_PFR_F25 UNIORM_PFR_F24, f25
#define UNIORM_PFR_F26 UNIORM_PFR_F25, f26
#define UNIORM_PFR_F27 UNIORM_PFR_F26, f27
#define UNIORM_PFR_F28 UNIORM_PFR_F27, f28
#define UNIORM_PFR_F29 UNIORM_PFR_F28, f29
#define UNIORM_PFR_F30 UNIORM_PFR_F29, f30
#define UNIORM_PFR_F31 UNIORM_PFR_F30, f31
#define UNIORM_PFR_F32 UNIORM_PFR_F31, f32
#define UNIORM_PFR_F33 UNIORM_PFR_F32, f33
#define UNIORM_PFR_F34 UNIORM_PFR_F33, f34
#define UNIORM_PFR_F35 UNIORM_PFR_F34, f35
#define UNIORM_PFR_F36 UNIORM_PFR_F35, f36
#define UNIORM_PFR_F37 UNIORM_PFR_F36, f37
#define UNIORM_PFR_F38 UNIORM_PFR_F37, f38
#define UNIORM_PFR_F39 UNIORM_PFR_F38, f39
#define UNIORM_PFR_F40 UNIORM_PFR_F39, f40
#define UNIORM_PFR_F41 UNIORM_PFR_F40, f41
#define UNIORM_PFR_F42 UNIORM_PFR_F41, f42
#define UNIORM_PFR_F43 UNIORM_PFR_F42, f43
#define UNIORM_PFR_F44 UNIORM_PFR_F43, f44
#define UNIORM_PFR_F45 UNIORM_PFR_F44, f45
#define UNIORM_PFR_F46 UNIORM_PFR_F45, f46
#define UNIORM_PFR_F47 UNIORM_PFR_F46, f47
#define UNIORM_PFR_F48 UNIORM_PFR_F47, f48
#define UNIORM_PFR_F49 UNIORM_PFR_F48, f49
#define UNIORM_PFR_F50 UNIORM_PFR_F49, f50
#define UNIORM_PFR_F51 UNIORM_PFR_F50, f51
#define UNIORM_PFR_F52 UNIORM_PFR_F51, f52
#define UNIORM_PFR_F53 UNIORM_PFR_F52, f53
#define UNIORM_PFR_F54 UNIORM_PFR_F53, f54
#define UNIORM_PFR_F55 UNIORM_PFR_F54, f55
#define UNIORM_PFR_F56 UNIORM_PFR_F55, f56
#define UNIORM_PFR_F57 UNIORM_PFR_F56, f57
#define UNIORM_PFR_F58 UNIORM_PFR_F57, f58
#define UNIORM_PFR_F59 UNIORM_PFR_F58, f59
#define UNIORM_PFR_F60 UNIORM_PFR_F59, f60
#define UNIORM_PFR_F61 UNIORM_PFR_F60, f61
#define UNIORM_PFR_F62 UNIORM_PFR_F61, f62
#define UNIORM_PFR_F63 UNIORM_PFR_F62, f63
#define UNIORM_PFR_F64 UNIORM_PFR_F63, f64

#define UNIORM_PFR_CASE(n, ...)     \
  else if constexpr (fields == n) { \
    auto& [__VA_ARGS__] = v;        \
    return std::tie(__VA_ARGS__);   \
  }

template <class T>
constexpr auto tie_aggregate(T& v) {
  constexpr std::size_t fields = field_count<std::remove_cvref_t<T>>();
  if constexpr (fields == 0) {
    return std::tie();
  }
  UNIORM_PFR_CASE(1, UNIORM_PFR_F1)
  UNIORM_PFR_CASE(2, UNIORM_PFR_F2)
  UNIORM_PFR_CASE(3, UNIORM_PFR_F3)
  UNIORM_PFR_CASE(4, UNIORM_PFR_F4)
  UNIORM_PFR_CASE(5, UNIORM_PFR_F5)
  UNIORM_PFR_CASE(6, UNIORM_PFR_F6)
  UNIORM_PFR_CASE(7, UNIORM_PFR_F7)
  UNIORM_PFR_CASE(8, UNIORM_PFR_F8)
  UNIORM_PFR_CASE(9, UNIORM_PFR_F9)
  UNIORM_PFR_CASE(10, UNIORM_PFR_F10)
  UNIORM_PFR_CASE(11, UNIORM_PFR_F11)
  UNIORM_PFR_CASE(12, UNIORM_PFR_F12)
  UNIORM_PFR_CASE(13, UNIORM_PFR_F13)
  UNIORM_PFR_CASE(14, UNIORM_PFR_F14)
  UNIORM_PFR_CASE(15, UNIORM_PFR_F15)
  UNIORM_PFR_CASE(16, UNIORM_PFR_F16)
  UNIORM_PFR_CASE(17, UNIORM_PFR_F17)
  UNIORM_PFR_CASE(18, UNIORM_PFR_F18)
  UNIORM_PFR_CASE(19, UNIORM_PFR_F19)
  UNIORM_PFR_CASE(20, UNIORM_PFR_F20)
  UNIORM_PFR_CASE(21, UNIORM_PFR_F21)
  UNIORM_PFR_CASE(22, UNIORM_PFR_F22)
  UNIORM_PFR_CASE(23, UNIORM_PFR_F23)
  UNIORM_PFR_CASE(24, UNIORM_PFR_F24)
  UNIORM_PFR_CASE(25, UNIORM_PFR_F25)
  UNIORM_PFR_CASE(26, UNIORM_PFR_F26)
  UNIORM_PFR_CASE(27, UNIORM_PFR_F27)
  UNIORM_PFR_CASE(28, UNIORM_PFR_F28)
  UNIORM_PFR_CASE(29, UNIORM_PFR_F29)
  UNIORM_PFR_CASE(30, UNIORM_PFR_F30)
  UNIORM_PFR_CASE(31, UNIORM_PFR_F31)
  UNIORM_PFR_CASE(32, UNIORM_PFR_F32)
  UNIORM_PFR_CASE(33, UNIORM_PFR_F33)
  UNIORM_PFR_CASE(34, UNIORM_PFR_F34)
  UNIORM_PFR_CASE(35, UNIORM_PFR_F35)
  UNIORM_PFR_CASE(36, UNIORM_PFR_F36)
  UNIORM_PFR_CASE(37, UNIORM_PFR_F37)
  UNIORM_PFR_CASE(38, UNIORM_PFR_F38)
  UNIORM_PFR_CASE(39, UNIORM_PFR_F39)
  UNIORM_PFR_CASE(40, UNIORM_PFR_F40)
  UNIORM_PFR_CASE(41, UNIORM_PFR_F41)
  UNIORM_PFR_CASE(42, UNIORM_PFR_F42)
  UNIORM_PFR_CASE(43, UNIORM_PFR_F43)
  UNIORM_PFR_CASE(44, UNIORM_PFR_F44)
  UNIORM_PFR_CASE(45, UNIORM_PFR_F45)
  UNIORM_PFR_CASE(46, UNIORM_PFR_F46)
  UNIORM_PFR_CASE(47, UNIORM_PFR_F47)
  UNIORM_PFR_CASE(48, UNIORM_PFR_F48)
  UNIORM_PFR_CASE(49, UNIORM_PFR_F49)
  UNIORM_PFR_CASE(50, UNIORM_PFR_F50)
  UNIORM_PFR_CASE(51, UNIORM_PFR_F51)
  UNIORM_PFR_CASE(52, UNIORM_PFR_F52)
  UNIORM_PFR_CASE(53, UNIORM_PFR_F53)
  UNIORM_PFR_CASE(54, UNIORM_PFR_F54)
  UNIORM_PFR_CASE(55, UNIORM_PFR_F55)
  UNIORM_PFR_CASE(56, UNIORM_PFR_F56)
  UNIORM_PFR_CASE(57, UNIORM_PFR_F57)
  UNIORM_PFR_CASE(58, UNIORM_PFR_F58)
  UNIORM_PFR_CASE(59, UNIORM_PFR_F59)
  UNIORM_PFR_CASE(60, UNIORM_PFR_F60)
  UNIORM_PFR_CASE(61, UNIORM_PFR_F61)
  UNIORM_PFR_CASE(62, UNIORM_PFR_F62)
  UNIORM_PFR_CASE(63, UNIORM_PFR_F63)
  UNIORM_PFR_CASE(64, UNIORM_PFR_F64)
  else {
    static_assert(
      fields <= max_aggregate_fields, "aggregate has too many fields");
  }
}

#undef UNIORM_PFR_CASE

template <class T, class F>
constexpr void for_each_field(T& v, F&& f) {
  std::apply([&](auto&... fs) { (f(fs), ...); }, tie_aggregate(v));
}

}  // namespace uniorm::detail
