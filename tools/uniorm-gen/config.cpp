#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace uniorm::gen {

namespace {

std::string fail(std::size_t line, std::string const& message) {
  return "line " + std::to_string(line) + ": " + message;
}

std::string strip_comment(std::string const& line) {
  bool in_quotes = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"') {
      in_quotes = !in_quotes;
    } else if (line[i] == '#' && !in_quotes) {
      return line.substr(0, i);
    }
  }
  return line;
}

std::string trim(std::string_view s) {
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return std::string(s.substr(begin, end - begin));
}

std::string to_upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c) { return std::toupper(c); });
  return s;
}

// Reads a possibly-quoted token starting at pos; advances pos past it.
// Quoted tokens keep their exact contents; bare tokens run to a delimiter.
std::string read_token(
  std::string const& s, std::size_t& pos, std::size_t line, char stop) {
  if (pos >= s.size()) {
    throw config_error(fail(line, "unexpected end of line"));
  }
  if (s[pos] == '"') {
    ++pos;
    std::string out;
    while (pos < s.size() && s[pos] != '"') {
      out.push_back(s[pos]);
      ++pos;
    }
    if (pos >= s.size()) {
      throw config_error(fail(line, "unterminated string"));
    }
    ++pos;  // closing quote
    return out;
  }
  std::string out;
  while (pos < s.size() && s[pos] != stop && s[pos] != '=' &&
         !std::isspace(static_cast<unsigned char>(s[pos]))) {
    out.push_back(s[pos]);
    ++pos;
  }
  if (out.empty()) {
    throw config_error(fail(line, "expected a token"));
  }
  return out;
}

// [a.b."c.d"] -> {"a", "b", "c.d"}
std::vector<std::string> parse_section(
  std::string const& line, std::size_t line_no) {
  if (line.back() != ']') {
    throw config_error(fail(line_no, "section must end with ']'"));
  }
  std::string body = line.substr(1, line.size() - 2);
  std::vector<std::string> parts;
  std::size_t pos = 0;
  for (;;) {
    while (pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[pos]))) {
      ++pos;
    }
    parts.push_back(read_token(body, pos, line_no, '.'));
    while (pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[pos]))) {
      ++pos;
    }
    if (pos >= body.size()) {
      break;
    }
    if (body[pos] != '.') {
      throw config_error(fail(line_no, "expected '.' in section name"));
    }
    ++pos;
  }
  return parts;
}

std::string parse_string_value(
  std::string const& value, std::size_t line_no, char const* context) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    throw config_error(
      fail(line_no, std::string(context) + " expects a quoted string"));
  }
  return value.substr(1, value.size() - 2);
}

bool parse_bool_value(
  std::string const& value, std::size_t line_no, char const* context) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  throw config_error(
    fail(line_no, std::string(context) + " expects true or false"));
}

struct key_value {
  std::string key;
  std::string value;
};

key_value parse_key_value(std::string const& line, std::size_t line_no) {
  std::size_t pos = 0;
  key_value kv;
  kv.key = read_token(line, pos, line_no, '.');
  while (
    pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
    ++pos;
  }
  if (pos >= line.size() || line[pos] != '=') {
    throw config_error(fail(line_no, "expected '=' after key"));
  }
  ++pos;
  kv.value = trim(std::string_view(line).substr(pos));
  if (kv.value.empty()) {
    throw config_error(fail(line_no, "expected a value"));
  }
  return kv;
}

table_config& table_entry(
  gen_config& cfg, std::string const& name, std::size_t line_no) {
  if (name.empty()) {
    throw config_error(fail(line_no, "empty table name in section"));
  }
  return cfg.tables[name];
}

void apply_setting(gen_config& cfg, std::vector<std::string> const& section,
  key_value const& kv, std::size_t line_no) {
  if (section.size() == 1 && section[0] == "types") {
    cfg.type_overrides[to_upper(kv.key)] =
      parse_string_value(kv.value, line_no, "[types] value");
    return;
  }
  if (section.size() >= 2 && section[0] == "tables") {
    table_config& t = table_entry(cfg, section[1], line_no);
    if (section.size() == 2) {
      if (kv.key == "class") {
        t.class_name = parse_string_value(kv.value, line_no, "class");
      } else if (kv.key == "skip") {
        t.skip = parse_bool_value(kv.value, line_no, "skip");
      } else {
        throw config_error(fail(line_no, "unknown table key: " + kv.key));
      }
      return;
    }
    if (section.size() == 4 && section[2] == "columns") {
      if (section[3].empty()) {
        throw config_error(fail(line_no, "empty column name in section"));
      }
      column_override& c = t.columns[section[3]];
      if (kv.key == "cpp_type") {
        c.cpp_type = parse_string_value(kv.value, line_no, "cpp_type");
      } else if (kv.key == "converter") {
        c.converter = parse_string_value(kv.value, line_no, "converter");
      } else {
        throw config_error(fail(line_no, "unknown column key: " + kv.key));
      }
      return;
    }
  }
  throw config_error(fail(line_no, "unknown section"));
}

}  // namespace

gen_config parse_config(std::string_view toml) {
  gen_config cfg;
  std::vector<std::string> section;
  std::size_t line_no = 0;
  std::size_t start = 0;
  for (;;) {
    std::size_t nl = toml.find('\n', start);
    std::string raw(toml.substr(start,
      nl == std::string_view::npos ? std::string_view::npos : nl - start));
    ++line_no;
    std::string line = trim(strip_comment(raw));
    if (!line.empty()) {
      if (line.front() == '[') {
        section = parse_section(line, line_no);
      } else {
        if (section.empty()) {
          throw config_error(fail(line_no, "key outside of any section"));
        }
        apply_setting(cfg, section, parse_key_value(line, line_no), line_no);
      }
    }
    if (nl == std::string_view::npos) {
      break;
    }
    start = nl + 1;
  }
  return cfg;
}

}  // namespace uniorm::gen
