#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "uniorm/error.hpp"

namespace uniorm::gen {

class config_error : public uniorm::uniorm_error {
public:
  using uniorm_error::uniorm_error;
};

struct column_override {
  std::optional<std::string> cpp_type;
  std::optional<std::string> converter;
};

struct table_config {
  std::optional<std::string> class_name;
  bool skip = false;
  std::unordered_map<std::string, column_override> columns;
};

struct gen_config {
  // Keys are stored uppercased so matching is case-insensitive; a key may
  // carry precision, e.g. "NUMERIC(10,2)".
  std::unordered_map<std::string, std::string> type_overrides;
  std::unordered_map<std::string, table_config> tables;
};

// Parses the TOML subset described in design.md section 6.4: [types],
// [tables.NAME], [tables.NAME.columns.COLUMN] sections with string/bool
// values, quoted keys, and # comments. Throws config_error with the line
// number on any malformed or unknown input.
gen_config parse_config(std::string_view toml);

}  // namespace uniorm::gen
