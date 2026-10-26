#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <uniorm/export.hpp>

namespace uniorm {

// Minimal per-database SQL generation quirks: identifier quoting and
// pagination syntax.
struct UNIORM_API dialect {
  // How generated SQL spells the identifiers it quotes. A server decides
  // whether a name is case-sensitive and what it stores. A dialect built by
  // hand keeps; detect() answers for a server whose habit it knows, and a
  // deployment can override that answer.
  enum class identifier_case { keep, lower, upper };
  enum class qualification { unsupported, schema, catalog };

  char quote_open = '"';
  char quote_close = '"';
  bool ansi_pagination = true;  // false => LIMIT/OFFSET style
  identifier_case identifiers = identifier_case::keep;
  qualification table_qualification = qualification::unsupported;

  // The name as this dialect will put it in SQL, quotes aside. Catalog reads
  // ask by it, so a miss is reported in the spelling the statement uses.
  std::string fold_identifier(std::string_view identifier) const;
  std::string quote_identifier(std::string_view identifier) const;
  std::string quote_exact_identifier(std::string_view identifier) const;
  std::string pagination(
    std::optional<std::size_t> limit, std::size_t offset) const;

  static dialect detect(std::string_view dbms_name);
};

}  // namespace uniorm
