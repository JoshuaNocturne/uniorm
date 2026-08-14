#include "naming.hpp"

#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace uniorm::gen {

namespace {

std::vector<std::string> split_words(std::string_view s) {
  std::vector<std::string> words;
  std::string current;
  auto flush = [&] {
    if (!current.empty()) {
      for (char& c : current) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      words.push_back(std::move(current));
      current.clear();
    }
  };
  for (char ch : s) {
    if (ch == '_' || ch == '-' || ch == ' ' || ch == '.') {
      flush();
      continue;
    }
    if (std::isupper(static_cast<unsigned char>(ch)) && !current.empty() &&
        (std::islower(static_cast<unsigned char>(current.back())) ||
          std::isdigit(static_cast<unsigned char>(current.back())))) {
      flush();  // userId -> user|Id
    } else if (std::islower(static_cast<unsigned char>(ch)) &&
               current.size() >= 2 &&
               std::isupper(static_cast<unsigned char>(current.back())) &&
               std::isupper(
                 static_cast<unsigned char>(current[current.size() - 2]))) {
      // HTTPCode -> HTTP|Code: the last upper starts the new word
      char last = current.back();
      current.pop_back();
      flush();
      current.push_back(last);
    }
    current.push_back(ch);
  }
  flush();
  return words;
}

bool is_cpp_keyword(std::string const& s) {
  static std::unordered_set<std::string> const keywords = {
    "alignas",
    "alignof",
    "and",
    "and_eq",
    "asm",
    "auto",
    "bitand",
    "bitor",
    "bool",
    "break",
    "case",
    "catch",
    "char",
    "char8_t",
    "char16_t",
    "char32_t",
    "class",
    "compl",
    "concept",
    "const",
    "consteval",
    "constexpr",
    "constinit",
    "const_cast",
    "continue",
    "co_await",
    "co_return",
    "co_yield",
    "decltype",
    "default",
    "delete",
    "do",
    "double",
    "dynamic_cast",
    "else",
    "enum",
    "explicit",
    "export",
    "extern",
    "false",
    "float",
    "for",
    "friend",
    "goto",
    "if",
    "inline",
    "int",
    "long",
    "mutable",
    "namespace",
    "new",
    "noexcept",
    "not",
    "not_eq",
    "nullptr",
    "operator",
    "or",
    "or_eq",
    "private",
    "protected",
    "public",
    "register",
    "reinterpret_cast",
    "requires",
    "return",
    "short",
    "signed",
    "sizeof",
    "static",
    "static_assert",
    "static_cast",
    "struct",
    "switch",
    "template",
    "this",
    "thread_local",
    "throw",
    "true",
    "try",
    "typedef",
    "typeid",
    "typename",
    "union",
    "unsigned",
    "using",
    "virtual",
    "void",
    "volatile",
    "wchar_t",
    "while",
    "xor",
    "xor_eq",
  };
  return keywords.count(s) != 0;
}

std::string fix_identifier(std::string id) {
  if (id.empty()) {
    return "_";
  }
  if (std::isdigit(static_cast<unsigned char>(id.front()))) {
    id.insert(id.begin(), '_');
  }
  if (is_cpp_keyword(id)) {
    id.push_back('_');
  }
  return id;
}

}  // namespace

std::string to_pascal_case(std::string_view identifier) {
  std::string out;
  for (std::string const& word : split_words(identifier)) {
    out.push_back(
      static_cast<char>(std::toupper(static_cast<unsigned char>(word[0]))));
    out.append(word, 1, std::string::npos);
  }
  return fix_identifier(std::move(out));
}

std::string to_camel_case(std::string_view identifier) {
  std::vector<std::string> words = split_words(identifier);
  std::string out;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i != 0) {
      out.push_back(static_cast<char>(
        std::toupper(static_cast<unsigned char>(words[i][0]))));
      out.append(words[i], 1, std::string::npos);
    } else {
      out.append(words[i]);
    }
  }
  return fix_identifier(std::move(out));
}

std::string to_unit_name(std::string_view identifier) {
  std::string out;
  for (char ch : identifier) {
    if (std::isalnum(static_cast<unsigned char>(ch))) {
      out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    } else {
      out.push_back('_');
    }
  }
  return fix_identifier(std::move(out));
}

}  // namespace uniorm::gen
