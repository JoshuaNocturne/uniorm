#pragma once

#include <optional>
#include <type_traits>

namespace uniorm::detail {

template <class T>
struct is_optional_trait : std::false_type {};
template <class T>
struct is_optional_trait<std::optional<T>> : std::true_type {};
template <class T>
inline constexpr bool is_optional_v =
  is_optional_trait<std::remove_cvref_t<T>>::value;

}  // namespace uniorm::detail
