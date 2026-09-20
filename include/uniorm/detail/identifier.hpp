#pragma once

// Private detail: the one case folding used wherever this library compares or
// emits an identifier name. Installed because the headers that fold are.

#include <algorithm>
#include <string>
#include <string_view>

namespace uniorm::detail {

// Folds ASCII letters only, so two names match the same way whatever locale
// the process runs under, and a name in another alphabet never folds onto a
// different name.
inline std::string fold_lower(std::string_view name) {
  std::string out(name);
  std::transform(out.begin(), out.end(), out.begin(), [](char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  });
  return out;
}

inline std::string fold_upper(std::string_view name) {
  std::string out(name);
  std::transform(out.begin(), out.end(), out.begin(), [](char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  });
  return out;
}

}  // namespace uniorm::detail
