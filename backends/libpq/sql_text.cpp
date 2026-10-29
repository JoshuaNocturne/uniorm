#include "sql_text.hpp"

#include <cctype>
#include <string>

#include <uniorm/backend/error.hpp>

namespace uniorm::libpq {
namespace {

[[noreturn]] void unterminated(char opening) {
  throw backend::backend_error("postgresql",
    "statement ends inside a quote or comment opened by " +
      std::string(1, opening),
    {});
}

// The characters PostgreSQL lets a word carry, so a mark sitting right after
// one belongs to that word rather than starting an operator of its own.
bool is_word_character(char c) {
  auto const byte = static_cast<unsigned char>(c);
  return std::isalnum(byte) != 0 || c == '_' || c == '$' || byte >= 0x80;
}

// A line comment runs to the newline, which the caller still copies so the
// next token keeps its separation.
std::size_t skip_line_comment(std::string_view sql, std::size_t index) {
  while (index < sql.size() && sql[index] != '\n') {
    ++index;
  }
  return index;
}

// Past a /* */ comment. PostgreSQL nests these, so the walk carries the depth
// each inner opening adds.
std::size_t skip_block_comment(std::string_view sql, std::size_t index) {
  std::size_t scan = index + 2;
  std::size_t depth = 1;
  for (;;) {
    if (scan + 1 >= sql.size()) {
      unterminated('/');
    }
    if (sql[scan] == '/' && sql[scan + 1] == '*') {
      ++depth;
      scan += 2;
      continue;
    }
    if (sql[scan] == '*' && sql[scan + 1] == '/') {
      --depth;
      if (depth == 0) {
        return scan + 2;
      }
      scan += 2;
      continue;
    }
    ++scan;
  }
}

// Past a run opened by `quote`, where doubling the quote keeps it part of what
// is quoted. An escape string adds backslash escapes on top of that, so a
// backslash takes the next character with it whatever that character is.
std::size_t skip_doubled(std::string_view sql, std::size_t index, char quote,
  bool backslash_escapes = false) {
  std::size_t scan = index + 1;
  for (;;) {
    if (scan >= sql.size()) {
      unterminated(quote);
    }
    if (backslash_escapes && sql[scan] == '\\') {
      scan += 2;
      continue;
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

// The characters a dollar-quote tag is spelled from. A dollar is not among
// them, which is what tells the delimiter that closes the tag from one that
// opens the next body.
bool is_tag_character(char c) {
  auto const byte = static_cast<unsigned char>(c);
  return std::isalnum(byte) != 0 || c == '_' || byte >= 0x80;
}

// The length of the delimiter that opens a dollar-quoted body at `index`, or
// zero when the dollar starts something else. An empty tag is two dollars;
// a named one is the word between them, which cannot begin with a digit -- so
// a position the caller already spelled as $1 is not read as a body.
std::size_t dollar_delimiter(std::string_view sql, std::size_t index) {
  std::size_t scan = index + 1;
  if (scan >= sql.size()) {
    return 0;
  }
  if (sql[scan] == '$') {
    return 2;
  }
  if (!is_tag_character(sql[scan]) ||
      std::isdigit(static_cast<unsigned char>(sql[scan])) != 0) {
    return 0;
  }
  while (scan < sql.size() && is_tag_character(sql[scan])) {
    ++scan;
  }
  if (scan >= sql.size() || sql[scan] != '$') {
    return 0;
  }
  return scan + 1 - index;
}

// Past a dollar-quoted body, handed the delimiter that opened it. The body
// ends at the next spelling of that same delimiter, and a function definition
// is the usual reason a statement carries one.
std::size_t skip_dollar_quoted(
  std::string_view sql, std::size_t index, std::size_t delimiter) {
  std::string_view const tag = sql.substr(index, delimiter);
  std::size_t const end = sql.find(tag, index + delimiter);
  if (end == std::string_view::npos) {
    unterminated('$');
  }
  return end + delimiter;
}

// Whether the E at `index` opens PostgreSQL's escape-string form. A word
// character leading it makes the E the last letter of that word instead.
bool leads_escape_string(std::string_view sql, std::size_t index) {
  return index == 0 || !is_word_character(sql[index - 1]);
}

void copy_span(
  std::string& out, std::string_view sql, std::size_t& index, std::size_t end) {
  out.append(sql.substr(index, end - index));
  index = end;
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
    if (character == '\'' || character == '"') {
      copy_span(out, sql, index, skip_doubled(sql, index, character));
      continue;
    }
    if ((character == 'e' || character == 'E') && index + 1 < sql.size() &&
        sql[index + 1] == '\'' && leads_escape_string(sql, index)) {
      std::size_t const end = skip_doubled(sql, index + 1, '\'', true);
      out.push_back(character);
      copy_span(out, sql, ++index, end);
      continue;
    }
    if (character == '$') {
      std::size_t const delimiter = dollar_delimiter(sql, index);
      if (delimiter > 0) {
        copy_span(out, sql, index,
          skip_dollar_quoted(sql, index, delimiter));
        continue;
      }
    }
    if (character == '?') {
      // A doubled mark is one the caller means the server to read: this
      // family spells its jsonb and geometric operators with a lone '?'.
      if (index + 1 < sql.size() && sql[index + 1] == '?') {
        out.push_back('?');
        index += 2;
        continue;
      }
      ++placeholder_count;
      out.push_back('$');
      out += std::to_string(placeholder_count);
      ++index;
      continue;
    }
    out.push_back(character);
    ++index;
  }
  return out;
}

}  // namespace uniorm::libpq
