#include "sql_text.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

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

// Past whatever the text's own rules hide from a structural reading: a
// comment, a quoted name or literal, an escape string, a dollar-quoted body.
// The position comes back unchanged where nothing of that kind starts there,
// which is how a caller tells a skipped construct from a structural character.
std::size_t skip_ignored(std::string_view sql, std::size_t index) {
  char const character = sql[index];
  if (character == '-' && index + 1 < sql.size() && sql[index + 1] == '-') {
    return skip_line_comment(sql, index);
  }
  if (character == '/' && index + 1 < sql.size() && sql[index + 1] == '*') {
    return skip_block_comment(sql, index);
  }
  if (character == '\'' || character == '"') {
    return skip_doubled(sql, index, character);
  }
  if ((character == 'e' || character == 'E') && index + 1 < sql.size() &&
      sql[index + 1] == '\'' && leads_escape_string(sql, index)) {
    return skip_doubled(sql, index + 1, '\'', true);
  }
  if (character == '$') {
    std::size_t const delimiter = dollar_delimiter(sql, index);
    if (delimiter > 0) {
      return skip_dollar_quoted(sql, index, delimiter);
    }
  }
  return index;
}

std::size_t skip_space(std::string_view sql, std::size_t index) {
  while (index < sql.size() &&
      std::isspace(static_cast<unsigned char>(sql[index])) != 0) {
    ++index;
  }
  return index;
}

std::size_t end_of_word(std::string_view sql, std::size_t index) {
  while (index < sql.size() && is_word_character(sql[index])) {
    ++index;
  }
  return index;
}

// A keyword read regardless of how it was cased, against a word that was not
// quoted -- a delimited name is never the VALUES this walk is looking for.
bool word_is(std::string_view word, std::string_view keyword) {
  if (word.size() != keyword.size()) {
    return false;
  }
  for (std::size_t index = 0; index < word.size(); ++index) {
    auto const one = static_cast<unsigned char>(word[index]);
    auto const other = static_cast<unsigned char>(keyword[index]);
    if (std::tolower(one) != other) {
      return false;
    }
  }
  return true;
}

// The number a `$n` at `index` spells, and the position just past it. Zero
// comes back where the dollar spells anything else, so an operator's mark or
// the opening of a quoted body is not read as a parameter.
std::size_t parameter_number_at(
  std::string_view sql, std::size_t index, std::size_t& number) {
  number = 0;
  if (index >= sql.size() || sql[index] != '$') {
    return 0;
  }
  std::size_t scan = index + 1;
  std::size_t digits = 0;
  while (scan < sql.size() &&
      std::isdigit(static_cast<unsigned char>(sql[scan])) != 0) {
    number = number * 10 + static_cast<std::size_t>(sql[scan] - '0');
    ++scan;
    ++digits;
  }
  if (digits == 0) {
    return 0;
  }
  return scan;
}

// Every parameter the text spells, in the order they appear, each with its
// position and the number it carries.
std::vector<std::pair<std::size_t, std::size_t>> parameters_of(
  std::string_view sql) {
  std::vector<std::pair<std::size_t, std::size_t>> out;
  std::size_t index = 0;
  while (index < sql.size()) {
    std::size_t const ignored_end = skip_ignored(sql, index);
    if (ignored_end > index) {
      index = ignored_end;
      continue;
    }
    std::size_t number = 0;
    std::size_t const after = parameter_number_at(sql, index, number);
    if (after > 0) {
      out.emplace_back(index, number);
      index = after;
      continue;
    }
    ++index;
  }
  return out;
}

// The position of the ')' that closes the group opening at `index`, or the end
// of the text where nothing does. Depth is kept because a group's entry may
// carry a parenthesized construct of its own.
std::size_t close_of_group(std::string_view sql, std::size_t index) {
  std::size_t scan = index;
  std::size_t depth = 0;
  while (scan < sql.size()) {
    std::size_t const ignored_end = skip_ignored(sql, scan);
    if (ignored_end > scan) {
      scan = ignored_end;
      continue;
    }
    if (sql[scan] == '(') {
      ++depth;
    } else if (sql[scan] == ')') {
      --depth;
      if (depth == 0) {
        return scan;
      }
    }
    ++scan;
  }
  return sql.size();
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

values_fold find_values_fold(
  std::string_view rewritten, std::size_t placeholder_count) {
  values_fold no_fold;
  if (placeholder_count == 0) {
    return no_fold;
  }

  // The first word decides whether this is an insert at all. A SELECT that
  // carries a VALUES of its own has no row of the caller's to repeat.
  std::size_t index = skip_space(rewritten, skip_ignored(rewritten, 0));
  if (index >= rewritten.size() || !is_word_character(rewritten[index])) {
    return no_fold;
  }
  std::size_t const lead_end = end_of_word(rewritten, index);
  if (!word_is(rewritten.substr(index, lead_end - index), "insert")) {
    return no_fold;
  }
  index = lead_end;

  // The one group after the one VALUES word. Two of either is a text that
  // already writes more than the caller's row, and a fold has no way to tell
  // the caller where its rows would end.
  std::size_t open = std::string_view::npos;
  while (index < rewritten.size()) {
    std::size_t const ignored_end = skip_ignored(rewritten, index);
    if (ignored_end > index) {
      index = ignored_end;
      continue;
    }
    if (!is_word_character(rewritten[index])) {
      ++index;
      continue;
    }
    std::size_t const end = end_of_word(rewritten, index);
    if (word_is(rewritten.substr(index, end - index), "values")) {
      std::size_t const after = skip_space(rewritten, end);
      if (open != std::string_view::npos || after >= rewritten.size() ||
          rewritten[after] != '(') {
        return no_fold;
      }
      open = after;
    }
    index = end;
  }
  if (open == std::string_view::npos) {
    return no_fold;
  }
  std::size_t const close = close_of_group(rewritten, open);
  if (close >= rewritten.size()) {
    return no_fold;
  }

  // Every parameter the statement declares belongs to the group. A binding
  // made outside it -- the WHERE of an upsert, a select of defaults -- is one
  // the fold would have to copy for every row, and no row owns it.
  auto const parameters = parameters_of(rewritten);
  if (parameters.size() != placeholder_count) {
    return no_fold;
  }
  for (auto const& parameter : parameters) {
    if (parameter.first <= open || parameter.first >= close) {
      return no_fold;
    }
  }

  // The group's entries, which have to be parameters and nothing else. An
  // expression written among them would be repeated with each row, which is
  // not what the caller asked to insert.
  std::size_t columns = 0;
  std::size_t scan = open + 1;
  for (;;) {
    scan = skip_space(rewritten, scan);
    if (scan >= close) {
      break;
    }
    if (columns > 0) {
      if (rewritten[scan] != ',') {
        return no_fold;
      }
      scan = skip_space(rewritten, scan + 1);
    }
    std::size_t number = 0;
    std::size_t const after = parameter_number_at(rewritten, scan, number);
    if (after == 0 || after > close || number != columns + 1) {
      return no_fold;
    }
    columns = number;
    scan = after;
  }
  if (columns == 0 || columns != parameters.size()) {
    return no_fold;
  }

  // What the group leaves behind. A comma or another group there is a second
  // row the text wrote for itself.
  std::size_t const after_group = skip_space(rewritten, close + 1);
  if (after_group < rewritten.size() &&
      (rewritten[after_group] == ',' || rewritten[after_group] == '(')) {
    return no_fold;
  }

  values_fold fold;
  fold.columns = columns;
  fold.head = std::string(rewritten.substr(0, open));
  fold.tail = std::string(rewritten.substr(close + 1));
  return fold;
}

std::string expand_values_fold(values_fold const& fold, std::size_t rows) {
  if (fold.columns == 0 || rows == 0) {
    return std::string();
  }
  std::string out = fold.head;
  for (std::size_t row = 0; row < rows; ++row) {
    if (row != 0) {
      out += ", ";
    }
    out.push_back('(');
    for (std::size_t column = 0; column < fold.columns; ++column) {
      if (column != 0) {
        out += ", ";
      }
      out.push_back('$');
      out += std::to_string(row * fold.columns + column + 1);
    }
    out.push_back(')');
  }
  out += fold.tail;
  return out;
}

}  // namespace uniorm::libpq
