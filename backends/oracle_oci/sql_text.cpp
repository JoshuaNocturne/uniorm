#include "sql_text.hpp"

#include <cctype>
#include <string>

#include <uniorm/backend/error.hpp>

namespace uniorm::oracle_oci {
namespace {

// Oracle's alternate quoting: q'X...X' where a bracket opener demands its own
// closer and any other character closes with itself.
char alternate_closer(char opener) {
  switch (opener) {
  case '[':
    return ']';
  case '{':
    return '}';
  case '<':
    return '>';
  case '(':
    return ')';
  default:
    return opener;
  }
}

[[noreturn]] void unterminated(char opening) {
  throw backend::backend_error("oracle",
    "statement ends inside a quote or comment opened by " +
      std::string(1, opening),
    {});
}

// A line comment runs to the newline, which the caller still copies so the
// next token keeps its separation.
std::size_t skip_line_comment(std::string_view sql, std::size_t index) {
  while (index < sql.size() && sql[index] != '\n') {
    ++index;
  }
  return index;
}

// Past a non-nesting /* */ comment, Oracle's only block comment form.
std::size_t skip_block_comment(std::string_view sql, std::size_t index) {
  std::size_t scan = index + 2;
  for (;;) {
    if (scan + 1 >= sql.size()) {
      unterminated('/');
    }
    if (sql[scan] == '*' && sql[scan + 1] == '/') {
      return scan + 2;
    }
    ++scan;
  }
}

// Past a '...' literal, where a doubled quote is one quote of the value. The
// same walk closes a "..." delimited identifier, so the quote is a parameter.
std::size_t skip_doubled_quote(
  std::string_view sql, std::size_t index, char quote) {
  std::size_t scan = index + 1;
  for (;;) {
    if (scan >= sql.size()) {
      unterminated(quote);
    }
    if (sql[scan] == quote) {
      if (scan + 1 < sql.size() && sql[scan + 1] == quote) {
        scan += 2;
        continue;
      }
      return scan + 1;
    }
    ++scan;
  }
}

// Past a q'X...X' literal, handed the index of the q.
std::size_t skip_alternate_quoted(std::string_view sql, std::size_t index) {
  if (index + 2 >= sql.size()) {
    unterminated('q');
  }
  char const closer = alternate_closer(sql[index + 2]);
  std::size_t scan = index + 3;
  for (;;) {
    if (scan + 1 >= sql.size()) {
      unterminated('q');
    }
    if (sql[scan] == closer && sql[scan + 1] == '\'') {
      return scan + 2;
    }
    ++scan;
  }
}

void copy_span(
  std::string& out, std::string_view sql, std::size_t& index, std::size_t end) {
  out.append(sql.substr(index, end - index));
  index = end;
}

// Case-insensitive match against an uppercase literal, so a leading keyword
// reads the same whether the caller typed SELECT or select.
bool word_is(std::string_view word, char const* expected) {
  std::size_t scan = 0;
  for (; expected[scan] != '\0'; ++scan) {
    if (scan >= word.size()) {
      return false;
    }
    auto const letter = static_cast<unsigned char>(word[scan]);
    if (std::toupper(letter) != expected[scan]) {
      return false;
    }
  }
  return scan == word.size();
}

}  // namespace

std::string rewrite_placeholders(
  std::string_view sql, std::size_t& placeholder_count) {
  std::string out;
  out.reserve(sql.size() + 8);
  placeholder_count = 0;
  std::size_t index = 0;
  while (index < sql.size()) {
    char const character = sql[index];
    if (character == '-' && index + 1 < sql.size() && sql[index + 1] == '-') {
      copy_span(out, sql, index, skip_line_comment(sql, index));
      continue;
    }
    if (character == '/' && index + 1 < sql.size() && sql[index + 1] == '*') {
      copy_span(out, sql, index, skip_block_comment(sql, index));
      continue;
    }
    if (character == '\'') {
      copy_span(out, sql, index, skip_doubled_quote(sql, index, '\''));
      continue;
    }
    if ((character == 'q' || character == 'Q') && index + 1 < sql.size() &&
        sql[index + 1] == '\'') {
      copy_span(out, sql, index, skip_alternate_quoted(sql, index));
      continue;
    }
    if (character == '"') {
      copy_span(out, sql, index, skip_doubled_quote(sql, index, '"'));
      continue;
    }
    if (character == '?') {
      ++placeholder_count;
      out.push_back(':');
      out += std::to_string(placeholder_count);
      ++index;
      continue;
    }
    out.push_back(character);
    ++index;
  }
  return out;
}

bool leads_with_query(std::string_view sql) {
  std::size_t index = 0;
  for (;;) {
    while (index < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[index]))) {
      ++index;
    }
    if (index + 1 < sql.size() && sql[index] == '-' && sql[index + 1] == '-') {
      index = skip_line_comment(sql, index);
      continue;
    }
    if (index + 1 < sql.size() && sql[index] == '/' && sql[index + 1] == '*') {
      index = skip_block_comment(sql, index);
      continue;
    }
    break;
  }
  std::size_t const head = index;
  while (index < sql.size() &&
         std::isalpha(static_cast<unsigned char>(sql[index]))) {
    ++index;
  }
  std::string_view const word = sql.substr(head, index - head);
  return word_is(word, "SELECT") || word_is(word, "WITH");
}

}  // namespace uniorm::oracle_oci
