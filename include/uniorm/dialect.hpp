#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "export.hpp"

namespace uniorm {

// Minimal per-database SQL generation quirks: identifier quoting and
// pagination syntax.
struct UNIORM_API dialect {
  char quote_open = '"';
  char quote_close = '"';
  bool ansi_pagination = true;  // false => LIMIT/OFFSET style

  std::string quote_identifier(std::string_view identifier) const;
  std::string pagination(
    std::optional<std::size_t> limit, std::size_t offset) const;

  static dialect detect(std::string_view dbms_name);
};

}  // namespace uniorm
