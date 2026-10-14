#include "ddl_meta.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "naming.hpp"

namespace uniorm::gen {
namespace {

using column_row = schema_meta::column_row;
using foreign_key_row = schema_meta::foreign_key_row;
using index_row = schema_meta::index_row;
using table_ref = schema_meta::table_ref;

constexpr std::size_t no_table = ~std::size_t{ 0 };

bool ident_start(char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool ident_char(char ch) {
  return ident_start(ch) || std::isdigit(static_cast<unsigned char>(ch)) != 0 ||
    ch == '$';
}

// Every skip that hunts for punctuation has to step over these: a ';' or a '('
// survives inside all of them.
bool quote_start(char ch) {
  return ch == '\'' || ch == '"' || ch == '`' || ch == '$';
}

char lower_char(char ch) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

bool all_digits(std::string const& text) {
  if (text.empty()) {
    return false;
  }
  for (char ch : text) {
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
      return false;
    }
  }
  return true;
}

// The types a script can spell, by the spelling it uses. What is missing here
// answers sql_type::other, which is what a live read answers with for a type
// the mapped set has no entry for: the generator then warns and binds text.
std::map<std::string, sql_type> const& type_table() {
  static std::map<std::string, sql_type> const table = {
    { "BIGINT", sql_type::bigint },
    { "BIGSERIAL", sql_type::bigint },
    { "BINARY", sql_type::binary },
    { "BIT", sql_type::binary },
    { "BIT VARYING", sql_type::varbinary },
    { "BLOB", sql_type::varbinary },
    { "BOOL", sql_type::boolean },
    { "BOOLEAN", sql_type::boolean },
    { "BPCHAR", sql_type::character },
    { "BYTEA", sql_type::varbinary },
    { "CHAR", sql_type::character },
    { "CHAR VARYING", sql_type::varchar },
    { "CHARACTER", sql_type::character },
    { "CHARACTER VARYING", sql_type::varchar },
    { "CLOB", sql_type::longvarchar },
    { "DATE", sql_type::date },
    { "DATETIME", sql_type::timestamp },
    { "DEC", sql_type::decimal },
    { "DECIMAL", sql_type::decimal },
    { "DOUBLE", sql_type::double_precision },
    { "DOUBLE PRECISION", sql_type::double_precision },
    { "ENUM", sql_type::varchar },
    { "FIXED", sql_type::decimal },
    { "FLOAT", sql_type::real },
    { "FLOAT4", sql_type::real },
    { "FLOAT8", sql_type::double_precision },
    { "IMAGE", sql_type::varbinary },
    { "INT", sql_type::integer },
    { "INT2", sql_type::smallint },
    { "INT4", sql_type::integer },
    { "INT8", sql_type::bigint },
    { "INTEGER", sql_type::integer },
    { "JSON", sql_type::longvarchar },
    { "JSONB", sql_type::longvarchar },
    { "LONG VARBINARY", sql_type::varbinary },
    { "LONG VARCHAR", sql_type::longvarchar },
    { "LONG VARCHARACTER", sql_type::varchar },
    { "LONGBLOB", sql_type::varbinary },
    { "LONGTEXT", sql_type::longvarchar },
    { "MEDIUMBLOB", sql_type::varbinary },
    { "MEDIUMINT", sql_type::integer },
    { "MEDIUMTEXT", sql_type::longvarchar },
    { "NATIONAL CHAR", sql_type::wchar },
    { "NATIONAL CHAR VARYING", sql_type::wvarchar },
    { "NATIONAL CHARACTER", sql_type::wchar },
    { "NATIONAL CHARACTER VARYING", sql_type::wvarchar },
    { "NCHAR", sql_type::wchar },
    { "NCHAR VARYING", sql_type::wvarchar },
    { "NCLOB", sql_type::wvarchar },
    { "NUMERIC", sql_type::decimal },
    { "NVARCHAR", sql_type::wvarchar },
    { "NVARCHAR2", sql_type::wvarchar },
    { "REAL", sql_type::real },
    { "SERIAL", sql_type::integer },
    { "SET", sql_type::varchar },
    { "SMALLDATETIME", sql_type::timestamp },
    { "SMALLINT", sql_type::smallint },
    { "SMALLSERIAL", sql_type::smallint },
    { "TEXT", sql_type::longvarchar },
    { "TIME", sql_type::time },
    { "TIME WITH TIME ZONE", sql_type::time },
    { "TIME WITHOUT TIME ZONE", sql_type::time },
    { "TIMESTAMP", sql_type::timestamp },
    { "TIMESTAMP WITH LOCAL TIME ZONE", sql_type::timestamp },
    { "TIMESTAMP WITH TIME ZONE", sql_type::timestamp },
    { "TIMESTAMP WITHOUT TIME ZONE", sql_type::timestamp },
    { "TIMESTAMPTZ", sql_type::timestamp },
    { "TIMETZ", sql_type::time },
    { "TINYBLOB", sql_type::varbinary },
    { "TINYINT", sql_type::smallint },
    { "TINYTEXT", sql_type::longvarchar },
    { "UUID", sql_type::guid },
    { "VARBINARY", sql_type::varbinary },
    { "VARCHAR", sql_type::varchar },
    { "VARCHAR2", sql_type::varchar },
    { "VARCHARACTER", sql_type::varchar },
    { "XML", sql_type::longvarchar },
    { "YEAR", sql_type::smallint },
  };
  return table;
}

bool known_type(std::string const& phrase) {
  return type_table().count(phrase) != 0;
}

// A phrase a longer spelling continues: "TIMESTAMP WITH" before "TIME ZONE"
// arrives. Some spellings put the arguments between the words, so the tail is
// read after them.
bool type_continues(std::string const& phrase) {
  for (auto const& entry : type_table()) {
    if (entry.first.size() > phrase.size() &&
      entry.first.compare(0, phrase.size(), phrase) == 0 &&
      entry.first[phrase.size()] == ' ') {
      return true;
    }
  }
  return false;
}

sql_type sql_type_of(std::string const& phrase) {
  auto const& table = type_table();
  auto found = table.find(phrase);
  if (found != table.end()) {
    return found->second;
  }
  std::size_t const space = phrase.find(' ');
  if (space != std::string::npos) {
    found = table.find(phrase.substr(0, space));
    if (found != table.end()) {
      return found->second;
    }
  }
  return sql_type::other;
}

std::int32_t clamp_size(std::string const& digits) {
  std::uint64_t value = 0;
  for (char ch : digits) {
    value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    if (value > static_cast<std::uint64_t>(INT32_MAX)) {
      return INT32_MAX;
    }
  }
  return static_cast<std::int32_t>(value);
}

class scanner {
public:
  struct spot {
    std::size_t offset = 0;
    std::size_t line = 1;
  };

  explicit scanner(std::string_view text) : text_(text) {}

  // Reads to the end of the script, blanks and comments included, so a trailing
  // comment is not mistaken for another statement.
  bool exhausted() {
    skip_blanks();
    return pos_ >= text_.size();
  }

  spot place() const { return spot{ pos_, line_ }; }
  void restore(spot const& at);

  void skip_blanks();
  bool peek_is(char ch);
  bool eat(char ch);
  void expect(char ch);
  bool peek_keyword(std::string_view word);
  bool eat_keyword(std::string_view word);
  void expect_keyword(std::string_view word);
  std::string peek_word();
  void take_word();
  bool eat_optional_name();
  std::string identifier();
  std::vector<std::string> qualified();
  std::string value();
  std::vector<std::string> name_list();
  std::vector<std::string> index_columns();
  // True at the ',' or ')' that ends one item of a parenthesized list, and at
  // the ';' or the end of the script that ends the statement around it.
  bool at_item_end();
  void skip_item();
  // The cursor sits on the '('; the matching ')' goes with it.
  void skip_balanced();
  void statement_end();
  std::string here();
  [[noreturn]] void fail(std::string const& message);

private:
  void advance(std::size_t count);
  void advance_to(std::size_t stop);
  bool line_comment(std::size_t at) const;
  void skip_line();
  void skip_block();
  void skip_quoted();

  std::string text_;
  std::size_t pos_ = 0;
  std::size_t line_ = 1;
};

void scanner::restore(spot const& at) {
  pos_ = at.offset;
  line_ = at.line;
}

void scanner::advance(std::size_t count) {
  while (count != 0 && pos_ < text_.size()) {
    if (text_[pos_] == '\n') {
      ++line_;
    }
    ++pos_;
    --count;
  }
}

void scanner::advance_to(std::size_t stop) {
  while (pos_ < stop && pos_ < text_.size()) {
    advance(1);
  }
}

bool scanner::line_comment(std::size_t at) const {
  return text_[at] == '#' ||
    (text_[at] == '-' && at + 1 < text_.size() && text_[at + 1] == '-');
}

void scanner::skip_line() {
  while (pos_ < text_.size() && text_[pos_] != '\n') {
    ++pos_;
  }
}

void scanner::skip_block() {
  std::size_t const start = line_;
  advance(2);
  while (pos_ < text_.size()) {
    if (text_[pos_] == '*' && pos_ + 1 < text_.size() &&
      text_[pos_ + 1] == '/') {
      advance(2);
      return;
    }
    advance(1);
  }
  line_ = start;
  fail("unterminated block comment");
}

void scanner::skip_blanks() {
  while (pos_ < text_.size()) {
    char const ch = text_[pos_];
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      advance(1);
    } else if (line_comment(pos_)) {
      skip_line();
    } else if (ch == '/' && pos_ + 1 < text_.size() &&
      text_[pos_ + 1] == '*') {
      skip_block();
    } else {
      return;
    }
  }
}

void scanner::skip_quoted() {
  char const open = text_[pos_];
  if (open == '$') {
    std::size_t tag = pos_ + 1;
    while (tag < text_.size() &&
      (std::isalnum(static_cast<unsigned char>(text_[tag])) != 0 ||
        text_[tag] == '_')) {
      ++tag;
    }
    if (tag >= text_.size() || text_[tag] != '$') {
      fail("'$' starts no quoted body");
    }
    std::string_view const marker = text_.substr(pos_, tag - pos_ + 1);
    std::size_t const stop = text_.find(marker, tag + 1);
    if (stop == std::string_view::npos) {
      fail("unterminated quoted body");
    }
    advance_to(stop + marker.size());
    return;
  }
  std::size_t const start = line_;
  advance(1);
  while (pos_ < text_.size()) {
    char const ch = text_[pos_];
    if (ch == '\\' && open == '\'') {
      advance(2);
      continue;
    }
    if (ch == open) {
      if (pos_ + 1 < text_.size() && text_[pos_ + 1] == open) {
        advance(2);
        continue;
      }
      advance(1);
      return;
    }
    advance(1);
  }
  line_ = start;
  fail("unterminated quoted text");
}

bool scanner::peek_is(char ch) {
  skip_blanks();
  return pos_ < text_.size() && text_[pos_] == ch;
}

bool scanner::eat(char ch) {
  skip_blanks();
  if (pos_ >= text_.size() || text_[pos_] != ch) {
    return false;
  }
  advance(1);
  return true;
}

void scanner::expect(char ch) {
  if (!eat(ch)) {
    fail("expected '" + std::string(1, ch) + "', found '" + here() + "'");
  }
}

bool scanner::peek_keyword(std::string_view word) {
  skip_blanks();
  if (pos_ + word.size() > text_.size()) {
    return false;
  }
  for (std::size_t offset = 0; offset < word.size(); ++offset) {
    if (lower_char(text_[pos_ + offset]) != lower_char(word[offset])) {
      return false;
    }
  }
  return pos_ + word.size() >= text_.size() ||
    !ident_char(text_[pos_ + word.size()]);
}

bool scanner::eat_keyword(std::string_view word) {
  if (!peek_keyword(word)) {
    return false;
  }
  advance(word.size());
  return true;
}

void scanner::expect_keyword(std::string_view word) {
  if (!eat_keyword(word)) {
    fail("expected '" + std::string(word) + "', found '" + here() + "'");
  }
}

std::string scanner::peek_word() {
  skip_blanks();
  if (pos_ >= text_.size() || !ident_start(text_[pos_])) {
    return {};
  }
  std::size_t end = pos_;
  while (end < text_.size() && ident_char(text_[end])) {
    ++end;
  }
  return std::string(text_.substr(pos_, end - pos_));
}

void scanner::take_word() {
  advance(peek_word().size());
}

bool scanner::eat_optional_name() {
  skip_blanks();
  if (pos_ < text_.size() && ident_start(text_[pos_])) {
    take_word();
    return true;
  }
  if (pos_ < text_.size() && (text_[pos_] == '`' || text_[pos_] == '"')) {
    identifier();
    return true;
  }
  return false;
}

std::string scanner::identifier() {
  skip_blanks();
  if (pos_ >= text_.size()) {
    fail("expected a name, found the end of the script");
  }
  char const open = text_[pos_];
  if (open == '`' || open == '"') {
    std::size_t const start = line_;
    advance(1);
    std::string out;
    while (true) {
      if (pos_ >= text_.size()) {
        line_ = start;
        fail("unterminated quoted name");
      }
      char const ch = text_[pos_];
      if (ch == open) {
        if (pos_ + 1 < text_.size() && text_[pos_ + 1] == open) {
          out += open;
          advance(2);
          continue;
        }
        advance(1);
        return out;
      }
      out += ch;
      advance(1);
    }
  }
  if (!ident_start(open)) {
    fail("expected a name, found '" + here() + "'");
  }
  std::size_t const start = pos_;
  while (pos_ < text_.size() && ident_char(text_[pos_])) {
    advance(1);
  }
  return std::string(text_.substr(start, pos_ - start));
}

std::vector<std::string> scanner::qualified() {
  std::vector<std::string> parts;
  parts.push_back(identifier());
  while (eat('.')) {
    parts.push_back(identifier());
  }
  return parts;
}

// One value as the script spells it: a literal, a number, or a name with the
// call parentheses, the cast and the bit or hex prefix a default can carry. It
// goes into the generated comment verbatim, so nothing here decodes it.
std::string scanner::value() {
  skip_blanks();
  if (pos_ >= text_.size()) {
    fail("expected a value, found the end of the script");
  }
  std::size_t const start = pos_;
  std::size_t end = pos_;
  char const ch = text_[pos_];
  if (ch == '\'' || ch == '$') {
    skip_quoted();
    end = pos_;
  } else if (std::isdigit(static_cast<unsigned char>(ch)) != 0 ||
    ((ch == '-' || ch == '+') && pos_ + 1 < text_.size() &&
      std::isdigit(static_cast<unsigned char>(text_[pos_ + 1])) != 0)) {
    advance(1);
    while (pos_ < text_.size() &&
      (std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0 ||
        text_[pos_] == '.' || text_[pos_] == 'x' || text_[pos_] == 'X')) {
      advance(1);
    }
    end = pos_;
  } else if (ch == '(') {
    // MySQL writes an expression default as a parenthesized tree.
    skip_balanced();
    end = pos_;
  } else if (ident_start(ch)) {
    identifier();
    end = pos_;
    while (true) {
      // A probe that skips blanks cannot leave them in the returned text, so
      // the dot has to follow the name at once.
      spot const before = place();
      if (!eat('.')) {
        restore(before);
        break;
      }
      identifier();
      end = pos_;
    }
  } else {
    fail("expected a value, found '" + here() + "'");
  }
  while (pos_ < text_.size()) {
    if (text_[pos_] == '\'' || text_[pos_] == '$') {
      skip_quoted();
    } else if (text_[pos_] == '(') {
      skip_balanced();
    } else if (text_[pos_] == ':' && pos_ + 1 < text_.size() &&
      text_[pos_ + 1] == ':') {
      advance(2);
      identifier();
    } else {
      break;
    }
    end = pos_;
  }
  return std::string(text_.substr(start, end - start));
}

std::vector<std::string> scanner::name_list() {
  expect('(');
  std::vector<std::string> names;
  while (true) {
    if (eat(')')) {
      break;
    }
    names.push_back(identifier());
    if (eat(',')) {
      continue;
    }
    expect(')');
    break;
  }
  return names;
}

std::vector<std::string> scanner::index_columns() {
  expect('(');
  std::vector<std::string> names;
  while (true) {
    if (eat(')')) {
      break;
    }
    // A term can carry a prefix length, an operator class or a direction after
    // the column; the name is all an index comment reports.
    names.push_back(identifier());
    skip_item();
    if (eat(',')) {
      continue;
    }
    expect(')');
    break;
  }
  return names;
}

bool scanner::at_item_end() {
  skip_blanks();
  if (pos_ >= text_.size()) {
    return true;
  }
  char const ch = text_[pos_];
  return ch == ',' || ch == ')' || ch == ';';
}

void scanner::skip_item() {
  std::size_t depth = 0;
  while (true) {
    skip_blanks();
    if (pos_ >= text_.size()) {
      return;
    }
    char const ch = text_[pos_];
    if (ch == ';' || (depth == 0 && (ch == ',' || ch == ')'))) {
      return;
    }
    if (quote_start(ch)) {
      skip_quoted();
      continue;
    }
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      if (depth != 0) {
        --depth;
      }
    }
    advance(1);
  }
}

void scanner::skip_balanced() {
  std::size_t const start = line_;
  expect('(');
  std::size_t depth = 1;
  while (pos_ < text_.size()) {
    char const ch = text_[pos_];
    if (quote_start(ch)) {
      skip_quoted();
      continue;
    }
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      --depth;
      if (depth == 0) {
        advance(1);
        return;
      }
    } else if (line_comment(pos_)) {
      skip_line();
      continue;
    } else if (ch == '/' && pos_ + 1 < text_.size() &&
      text_[pos_ + 1] == '*') {
      skip_block();
      continue;
    }
    advance(1);
  }
  line_ = start;
  fail("unterminated parenthesized list");
}

void scanner::statement_end() {
  std::size_t depth = 0;
  // A routine body closes its own statements with ';', so the BEGIN ... END
  // holding them is counted alongside the parentheses.
  std::size_t blocks = 0;
  while (true) {
    skip_blanks();
    if (pos_ >= text_.size()) {
      return;
    }
    char const ch = text_[pos_];
    if (ch == ';') {
      advance(1);
      if (depth == 0 && blocks == 0) {
        return;
      }
      continue;
    }
    if (quote_start(ch)) {
      skip_quoted();
      continue;
    }
    if (ident_start(ch)) {
      std::string const word = peek_word();
      std::string const name = fold_lower(word);
      if (name == "begin") {
        ++blocks;
      } else if (name == "end" && blocks != 0) {
        --blocks;
      }
      advance(word.size());
      continue;
    }
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      if (depth != 0) {
        --depth;
      }
    }
    advance(1);
  }
}

// The token at the cursor, for a message. It consumes what it reports, so every
// caller is about to throw.
std::string scanner::here() {
  skip_blanks();
  if (pos_ >= text_.size()) {
    return "the end of the script";
  }
  std::size_t const start = pos_;
  if (ident_start(text_[pos_])) {
    while (pos_ < text_.size() && ident_char(text_[pos_])) {
      advance(1);
    }
    return std::string(text_.substr(start, pos_ - start));
  }
  if (text_[pos_] == '\'') {
    skip_quoted();
    return std::string(text_.substr(start, pos_ - start));
  }
  std::string out(1, text_[pos_]);
  advance(1);
  return out;
}

void scanner::fail(std::string const& message) {
  throw ddl_error(
    "ddl: line " + std::to_string(line_) + ": " + message);
}

// What a script asks for that a mapping cannot carry. Statements of one kind
// are counted and reported once: a dump carries thousands of the same line,
// and the warnings worth reading would drown in them.
class warning_sink {
public:
  explicit warning_sink(std::vector<std::string>* target) : into_(target) {}

  void note(std::string const& message) {
    if (into_ != nullptr) {
      into_->push_back(message);
    }
  }

  void ignore(std::string const& what) {
    if (into_ != nullptr) {
      ++counts_[what];
    }
  }

  void flush() {
    for (auto const& entry : counts_) {
      note(entry.second == 1
          ? "ignored " + entry.first
          : "ignored " + std::to_string(entry.second) + " x " + entry.first);
    }
  }

private:
  std::vector<std::string>* into_;
  std::map<std::string, std::size_t> counts_;
};

// A foreign key that names no referenced columns: the rule is that it points at
// the target's primary key, which another statement may only declare later.
struct pending_key {
  std::size_t owner = 0;
  std::vector<std::string> target;
  std::vector<std::string> local;
};

struct reader {
  ddl_snapshot& out;
  warning_sink& sink;
  std::vector<pending_key>& pending;
  std::size_t slot = 0;

  ddl_table& table() { return out.tables[slot]; }
};

std::size_t find_table(
  std::vector<ddl_table> const& tables, std::vector<std::string> const& parts) {
  std::string const name = fold_lower(parts.back());
  for (std::size_t index = 0; index < tables.size(); ++index) {
    ddl_table const& table = tables[index];
    if (fold_lower(table.ref.name) != name) {
      continue;
    }
    if (parts.size() > 1 &&
      (!table.ref.schema.empty() || !table.ref.catalog.empty())) {
      std::string const qualifier = fold_lower(parts[parts.size() - 2]);
      if (fold_lower(table.ref.schema) != qualifier &&
        fold_lower(table.ref.catalog) != qualifier) {
        continue;
      }
    }
    return index;
  }
  return no_table;
}

// A script qualifies a table with one name — a schema, or a MySQL database — or
// with two. Which catalog word carries the rest is a live backend's business;
// here one qualifier lands in schema, the word --schema narrows.
ddl_table fresh_table(std::vector<std::string> const& parts) {
  ddl_table table;
  if (parts.size() == 1) {
    table.ref.name = parts[0];
  } else if (parts.size() == 2) {
    table.ref.schema = parts[0];
    table.ref.name = parts[1];
  } else {
    table.ref.catalog = parts[0];
    table.ref.schema = parts[1];
    table.ref.name = parts[2];
  }
  return table;
}

std::vector<std::string> table_name(scanner& in) {
  std::vector<std::string> parts = in.qualified();
  if (parts.size() > 3) {
    in.fail("'" + parts[0] + "' is not a table name: three parts at most");
  }
  return parts;
}

// An index the script leaves unnamed still has to be reported, and its columns
// spell the name the comment prints.
std::string index_name(
  std::string const& given, std::vector<std::string> const& columns,
  char const* fallback) {
  if (!given.empty()) {
    return given;
  }
  std::string out = fallback;
  for (std::string const& column : columns) {
    out += "_";
    out += column;
  }
  return out;
}

void mark_key(ddl_table& table, std::string const& column) {
  if (std::find(table.primary_key.begin(), table.primary_key.end(), column) ==
    table.primary_key.end()) {
    table.primary_key.push_back(column);
  }
}

// A primary key is not nullable in any catalog that reports one, whether the
// script said so on the column or in a constraint of its own.
void apply_keys(ddl_table& table) {
  for (std::string const& column : table.primary_key) {
    for (column_row& row : table.columns) {
      if (row.shape.name == column ||
        fold_lower(row.shape.name) == fold_lower(column)) {
        row.shape.nullable = false;
      }
    }
  }
}

// KEY, INDEX and UNIQUE also open a column definition, because a dialect lets
// them name a column without quoting it. What settles which one it is: a key
// list has to open a parenthesis.
bool opens_a_key_list(scanner& in, char const* lead) {
  scanner::spot const before = in.place();
  bool shaped = false;
  if (in.eat_keyword(lead)) {
    for (int term = 0; term < 3; ++term) {
      if (in.peek_is('(')) {
        shaped = true;
        break;
      }
      if (!in.eat_optional_name()) {
        break;
      }
    }
  }
  in.restore(before);
  return shaped;
}

bool constraint_lead(scanner& in) {
  if (in.peek_keyword("primary") || in.peek_keyword("foreign") ||
    in.peek_keyword("check") || in.peek_keyword("exclude") ||
    in.peek_keyword("fulltext") || in.peek_keyword("spatial")) {
    return true;
  }
  return opens_a_key_list(in, "unique") || opens_a_key_list(in, "key") ||
    opens_a_key_list(in, "index");
}

void read_references(
  scanner& in, reader& rd, std::vector<std::string> const& local) {
  // An inline column definition eats the keyword before it calls; a table
  // constraint arrives with it still unread.
  in.eat_keyword("references");
  std::vector<std::string> const target = table_name(in);
  std::vector<std::string> columns;
  if (in.peek_is('(')) {
    columns = in.name_list();
  }
  if (columns.empty()) {
    rd.pending.push_back(pending_key{ rd.slot, target, local });
  } else {
    if (columns.size() != local.size()) {
      in.fail("a FOREIGN KEY of " + std::to_string(local.size()) +
        " columns references " + std::to_string(columns.size()) +
        " columns in '" + target.back() + "'");
    }
    ddl_table& table = rd.table();
    for (std::size_t index = 0; index < columns.size(); ++index) {
      table.foreign_keys.push_back(
        foreign_key_row{ target.back(), columns[index], local[index] });
    }
  }
  in.skip_item();
}

void read_key_columns_then_references(scanner& in, reader& rd) {
  if (in.peek_is('(')) {
    read_references(in, rd, in.name_list());
    return;
  }
  // MySQL's shorthand: a key on the one column the constraint is about.
  std::vector<std::string> const local;
  read_references(in, rd, local);
}

void read_arguments(scanner& in, column_row& row) {
  std::size_t slot = 0;
  while (!in.eat(')')) {
    std::string const item = in.value();
    if (slot == 0 && all_digits(item)) {
      row.size = clamp_size(item);
    } else if (slot == 1 && all_digits(item)) {
      row.decimals = static_cast<std::int16_t>(clamp_size(item));
    }
    ++slot;
    if (in.eat(',')) {
      continue;
    }
    in.expect(')');
    break;
  }
}

void read_type(scanner& in, column_row& row, std::string const& column) {
  // A user type can arrive qualified: public.citext names the type citext,
  // which is the word a [types] override is keyed on and what a live read
  // reports.
  std::string phrase = fold_upper(in.qualified().back());
  bool const args_read = in.eat('(');
  if (args_read) {
    read_arguments(in, row);
  }
  while (true) {
    std::string const word = in.peek_word();
    if (word.empty()) {
      break;
    }
    std::string const next = phrase + " " + fold_upper(word);
    if (!known_type(next) && !type_continues(next)) {
      break;
    }
    in.take_word();
    phrase = next;
  }
  // CHARACTER VARYING(8) puts its arguments after the whole phrase,
  // TIMESTAMP(3) WITH TIME ZONE before the tail of it.
  if (!args_read && in.eat('(')) {
    read_arguments(in, row);
  }
  while (in.eat_keyword("unsigned") || in.eat_keyword("signed") ||
    in.eat_keyword("zerofill")) {
  }
  if (in.peek_is('[')) {
    in.fail("column '" + column + "' has type '" + phrase +
      "[]': the v1 binding set carries no arrays");
  }
  row.type_name = phrase;
  row.shape.type = sql_type_of(phrase);
  // No catalog reports a serial column nullable, and it is not: the sequence
  // fills it.
  if (phrase == "SERIAL" || phrase == "BIGSERIAL" || phrase == "SMALLSERIAL") {
    row.shape.nullable = false;
  }
}

// GENERATED ... AS (expr) and MySQL's short form of it: the column reads, and a
// write to it is the database's to refuse, which nothing in a generated struct
// says about the member.
void read_generated(
  scanner& in, std::string const& column, warning_sink& sink) {
  bool expression = false;
  if (!in.eat_keyword("always")) {
    in.eat_keyword("by");
    in.eat_keyword("default");
  }
  if (in.eat_keyword("as")) {
    if (in.eat_keyword("identity")) {
      // A sequence-backed column: ordinary to read, ordinary to leave out of a
      // write, so nothing to report.
    } else if (in.peek_is('(')) {
      in.skip_balanced();
      expression = true;
    } else {
      in.fail("unsupported GENERATED clause on column '" + column + "'");
    }
  } else if (in.peek_is('(')) {
    in.skip_balanced();
    expression = true;
  }
  in.eat_keyword("stored");
  in.eat_keyword("virtual");
  if (expression) {
    sink.note("generated column '" + column +
      "' is emitted as a plain column: reading it works, writing it does not");
  }
}

void read_column_def(scanner& in, reader& rd) {
  column_row row;
  row.shape.name = in.identifier();
  row.shape.nullable = true;
  ddl_table& table = rd.table();
  read_type(in, row, row.shape.name);
  while (!in.at_item_end()) {
    if (in.eat_keyword("not")) {
      if (in.eat_keyword("null")) {
        row.shape.nullable = false;
      } else {
        in.expect_keyword("enforced");
      }
      continue;
    }
    if (in.eat_keyword("null")) {
      row.shape.nullable = true;
      continue;
    }
    if (in.eat_keyword("default")) {
      row.default_value = in.value();
      continue;
    }
    if (in.eat_keyword("primary")) {
      in.expect_keyword("key");
      row.shape.nullable = false;
      mark_key(table, row.shape.name);
      continue;
    }
    if (in.eat_keyword("unique")) {
      in.eat_keyword("key");
      in.eat_keyword("index");
      std::vector<std::string> const only{ row.shape.name };
      table.indexes.push_back(
        index_row{ index_name(std::string(), only, "unique"), only, true });
      continue;
    }
    if (in.eat_keyword("references")) {
      std::vector<std::string> const only{ row.shape.name };
      read_references(in, rd, only);
      continue;
    }
    if (in.eat_keyword("collate")) {
      if (in.eat_keyword("if")) {
        in.eat_keyword("exists");
      }
      in.identifier();
      continue;
    }
    if (in.eat_keyword("character")) {
      in.expect_keyword("set");
      in.identifier();
      continue;
    }
    if (in.eat_keyword("charset")) {
      in.eat('=');
      in.identifier();
      continue;
    }
    if (in.eat_keyword("comment")) {
      in.value();
      continue;
    }
    if (in.eat_keyword("on")) {
      in.expect_keyword("update");
      in.value();
      continue;
    }
    if (in.eat_keyword("generated") || in.eat_keyword("as")) {
      read_generated(in, row.shape.name, rd.sink);
      continue;
    }
    if (in.eat_keyword("constraint")) {
      in.identifier();
      continue;
    }
    if (in.eat_keyword("check")) {
      if (in.peek_is('(')) {
        in.skip_balanced();
      }
      continue;
    }
    if (in.eat_keyword("storage") || in.eat_keyword("compression") ||
      in.eat_keyword("srid")) {
      in.value();
      continue;
    }
    if (in.eat_keyword("initially")) {
      in.eat_keyword("deferred");
      in.eat_keyword("immediate");
      continue;
    }
    if (in.eat_keyword("auto_increment") || in.eat_keyword("invisible") ||
      in.eat_keyword("visible") || in.eat_keyword("mandatory") ||
      in.eat_keyword("deferrable")) {
      continue;
    }
    // A column added by ALTER TABLE also says where in the table it goes;
    // column order in the script is what the header follows.
    if (in.eat_keyword("after")) {
      in.identifier();
      continue;
    }
    if (in.eat_keyword("first")) {
      continue;
    }
    in.fail("unsupported definition of column '" + row.shape.name +
      "': found '" + in.here() + "'");
  }
  table.columns.push_back(std::move(row));
}

void read_constraint(scanner& in, reader& rd) {
  ddl_table& table = rd.table();
  if (in.eat_keyword("primary")) {
    in.expect_keyword("key");
    for (std::string const& column : in.name_list()) {
      mark_key(table, column);
    }
    in.skip_item();
    return;
  }
  if (in.eat_keyword("unique")) {
    in.eat_keyword("key");
    in.eat_keyword("index");
    in.eat_keyword("nulls");
    in.eat_keyword("not");
    in.eat_keyword("distinct");
    std::string name;
    if (!in.peek_is('(')) {
      name = in.identifier();
    }
    std::vector<std::string> const columns = in.index_columns();
    table.indexes.push_back(
      index_row{ index_name(name, columns, "unique"), columns, true });
    in.skip_item();
    return;
  }
  if (in.eat_keyword("key") || in.eat_keyword("index")) {
    std::string name;
    if (!in.peek_is('(')) {
      name = in.identifier();
    }
    std::vector<std::string> const columns = in.index_columns();
    table.indexes.push_back(
      index_row{ index_name(name, columns, "index"), columns, false });
    in.skip_item();
    return;
  }
  if (in.eat_keyword("foreign")) {
    in.expect_keyword("key");
    read_key_columns_then_references(in, rd);
    return;
  }
  if (in.eat_keyword("check")) {
    if (in.peek_is('(')) {
      in.skip_balanced();
    }
    in.skip_item();
    rd.sink.ignore("a non-value constraint on table '" + table.ref.name + "'");
    return;
  }
  if (in.eat_keyword("fulltext") || in.eat_keyword("spatial") ||
    in.eat_keyword("exclude")) {
    in.skip_item();
    rd.sink.ignore("a non-value constraint on table '" + table.ref.name + "'");
    return;
  }
  in.fail("unsupported table constraint on '" + table.ref.name + "': found '" +
    in.here() + "'");
}

void read_table_item(scanner& in, reader& rd) {
  if (in.eat_keyword("constraint")) {
    if (!constraint_lead(in)) {
      in.identifier();
    }
    read_constraint(in, rd);
    return;
  }
  if (constraint_lead(in)) {
    read_constraint(in, rd);
    return;
  }
  read_column_def(in, rd);
}

void read_table_items(scanner& in, ddl_snapshot& out, std::size_t slot,
  warning_sink& sink, std::vector<pending_key>& pending) {
  reader rd{ out, sink, pending, slot };
  if (in.eat_keyword("like")) {
    in.fail("CREATE TABLE ... LIKE copies a table this script does not give");
  }
  in.expect('(');
  while (true) {
    if (in.eat(')')) {
      break;
    }
    read_table_item(in, rd);
    if (in.eat(',')) {
      continue;
    }
    in.expect(')');
    break;
  }
  apply_keys(out.tables[slot]);
  // Everything past the closing parenthesis is a table option: an engine, a
  // character set, a partition rule. None of it changes what a mapping is.
  in.statement_end();
}

ddl_table* target_table(
  ddl_snapshot& out, std::vector<std::string> const& parts) {
  std::size_t const slot = find_table(out.tables, parts);
  return slot == no_table ? nullptr : &out.tables[slot];
}

void read_create_index(
  scanner& in, ddl_snapshot& out, warning_sink& sink, bool unique) {
  in.eat_keyword("if");
  in.eat_keyword("not");
  in.eat_keyword("exists");
  in.eat_keyword("concurrently");
  std::string const name = in.identifier();
  if (!in.eat_keyword("on")) {
    in.fail("expected ON in the definition of index '" + name + "'");
  }
  std::vector<std::string> const parts = table_name(in);
  if (in.eat_keyword("using")) {
    in.identifier();
  }
  std::vector<std::string> const columns = in.index_columns();
  ddl_table* table = target_table(out, parts);
  if (table == nullptr) {
    sink.note("index '" + name + "' is on table '" + parts.back() +
      "', which this script does not create");
  } else {
    table->indexes.push_back(
      index_row{ index_name(name, columns, "index"), columns, unique });
  }
  in.statement_end();
}

void read_create_table(
  scanner& in, ddl_snapshot& out, warning_sink& sink,
  std::vector<pending_key>& pending) {
  in.eat_keyword("if");
  in.eat_keyword("not");
  in.eat_keyword("exists");
  std::vector<std::string> const parts = table_name(in);
  std::size_t slot = find_table(out.tables, parts);
  if (slot == no_table) {
    slot = out.tables.size();
    out.tables.push_back(fresh_table(parts));
  } else {
    sink.note("table '" + parts.back() +
      "' is created twice: the last definition is the one read");
    out.tables[slot] = fresh_table(parts);
  }
  read_table_items(in, out, slot, sink, pending);
}

void read_added_columns(scanner& in, reader& rd) {
  bool const grouped = in.eat('(');
  read_column_def(in, rd);
  while (grouped && in.eat(',')) {
    read_column_def(in, rd);
  }
  if (grouped) {
    in.expect(')');
  }
}

void read_alter(
  scanner& in, ddl_snapshot& out, warning_sink& sink,
  std::vector<pending_key>& pending) {
  if (!in.eat_keyword("table")) {
    sink.ignore("ALTER " + in.here());
    in.statement_end();
    return;
  }
  in.eat_keyword("if");
  // pg_dump names the table it alters through ONLY; a table actually called
  // only loses this one statement to the keyword.
  in.eat_keyword("only");
  in.eat_keyword("exists");
  std::vector<std::string> const parts = table_name(in);
  std::size_t const slot = find_table(out.tables, parts);
  if (slot == no_table) {
    sink.note("ALTER TABLE names '" + parts.back() +
      "', which this script does not create");
    in.statement_end();
    return;
  }
  reader rd{ out, sink, pending, slot };
  while (in.eat_keyword("add")) {
    if (in.eat_keyword("column")) {
      read_added_columns(in, rd);
    } else if (constraint_lead(in) || in.peek_keyword("constraint")) {
      if (in.eat_keyword("constraint") && !constraint_lead(in)) {
        in.identifier();
      }
      read_constraint(in, rd);
    } else {
      read_added_columns(in, rd);
    }
    if (!in.eat(',')) {
      break;
    }
  }
  // An action that adds nothing changes the shape in a way a snapshot of a
  // script cannot follow, so it is reported instead of applied.
  while (!in.at_item_end()) {
    sink.ignore("ALTER TABLE " + parts.back() + " " + in.here() + " action");
    in.skip_item();
    if (!in.eat(',')) {
      break;
    }
  }
  apply_keys(rd.table());
  in.statement_end();
}

// Statements that carry no schema: their own subject is data, a session setting
// or an administration task, and nothing a mapping is built from goes missing.
bool carries_no_schema(std::string const& word) {
  static char const* const quiet[] = { "insert",    "replace",   "update",
    "delete",      "merge",     "set",       "begin",   "start",
    "commit",      "rollback",  "truncate",  "drop",    "lock",
    "unlock",      "grant",     "revoke",    "flush",   "analyze",
    "analyse",     "optimize",  "repair",    "show",    "describe",
    "explain",     "call",      "do",        "pragma",  "reset",
    "kill",        "handler",   "savepoint", "prepare", "execute",
    "deallocate",  "table",     "values",    "select",  "with",
    "go",          "declare",   "print",     "sleep",   "refresh",
    "vacuum",      "cluster",   "reindex",   "listen",  "notify",
    "unlisten",    "checkpoint" };
  for (char const* candidate : quiet) {
    if (word == candidate) {
      return true;
    }
  }
  return false;
}

void read_create(scanner& in, ddl_snapshot& out, warning_sink& sink,
  std::vector<pending_key>& pending) {
  in.eat_keyword("or");
  in.eat_keyword("replace");
  in.eat_keyword("temp");
  in.eat_keyword("temporary");
  in.eat_keyword("global");
  in.eat_keyword("local");
  in.eat_keyword("unlogged");
  if (in.eat_keyword("table")) {
    read_create_table(in, out, sink, pending);
    return;
  }
  bool const unique = in.eat_keyword("unique");
  if (in.eat_keyword("index")) {
    read_create_index(in, out, sink, unique);
    return;
  }
  if (in.eat_keyword("database") || in.eat_keyword("schema")) {
    in.eat_keyword("if");
    in.eat_keyword("not");
    in.eat_keyword("exists");
    in.qualified();
    in.statement_end();
    return;
  }
  sink.ignore("CREATE " + fold_upper(in.here()));
  in.statement_end();
}

void parse_script(scanner& in, ddl_snapshot& out, warning_sink& sink,
  std::vector<pending_key>& pending) {
  while (!in.exhausted()) {
    if (in.eat(';')) {
      continue;
    }
    if (in.eat_keyword("create")) {
      read_create(in, out, sink, pending);
      continue;
    }
    if (in.eat_keyword("alter")) {
      read_alter(in, out, sink, pending);
      continue;
    }
    if (in.eat_keyword("use")) {
      in.eat_keyword("database");
      out.database = in.identifier();
      in.statement_end();
      continue;
    }
    std::string const word = fold_lower(in.here());
    if (!carries_no_schema(word)) {
      sink.ignore("statement '" + word + "'");
    }
    in.statement_end();
  }
}

void resolve_pending_keys(ddl_snapshot& out,
  std::vector<pending_key> const& pending, warning_sink& sink) {
  for (pending_key const& key : pending) {
    ddl_table& owner = out.tables[key.owner];
    ddl_table const* target = target_table(out, key.target);
    if (target == nullptr || target->primary_key.size() != key.local.size()) {
      sink.note("the FOREIGN KEY on '" + owner.ref.name + "' references '" +
        key.target.back() + "', whose columns this script does not name");
      continue;
    }
    for (std::size_t index = 0; index < key.local.size(); ++index) {
      owner.foreign_keys.push_back(foreign_key_row{ key.target.back(),
        target->primary_key[index], key.local[index] });
    }
  }
}

}  // namespace

ddl_snapshot parse_ddl(
  std::string_view script, std::vector<std::string>* warnings) {
  scanner in(script);
  warning_sink sink(warnings);
  ddl_snapshot out;
  std::vector<pending_key> pending;
  parse_script(in, out, sink, pending);
  resolve_pending_keys(out, pending, sink);
  sink.flush();
  return out;
}

ddl_meta::ddl_meta(
  std::string_view script, std::vector<std::string>* warnings) {
  ddl_snapshot parsed = parse_ddl(script, warnings);
  tables_ = std::move(parsed.tables);
  database_ = std::move(parsed.database);
}

std::string ddl_meta::database_name() {
  return database_;
}

std::vector<schema_meta::table_row> ddl_meta::tables(
  std::string_view catalog, std::string_view schema) {
  std::vector<table_row> rows;
  for (ddl_table const& table : tables_) {
    // A script that leaves a table unqualified says nothing about where it
    // lives, so a narrowing that names a place cannot exclude it.
    if (!catalog.empty() && !table.ref.catalog.empty() &&
      fold_lower(catalog) != fold_lower(table.ref.catalog)) {
      continue;
    }
    if (!schema.empty() && !table.ref.schema.empty() &&
      fold_lower(schema) != fold_lower(table.ref.schema)) {
      continue;
    }
    rows.push_back(
      table_row{ table.ref.catalog, table.ref.schema, table.ref.name });
  }
  return rows;
}

ddl_table const* ddl_meta::find(table_ref const& ref) const {
  for (ddl_table const& table : tables_) {
    if (table.ref.name == ref.name && table.ref.schema == ref.schema &&
      table.ref.catalog == ref.catalog) {
      return &table;
    }
  }
  return nullptr;
}

std::vector<schema_meta::column_row> ddl_meta::table_columns(
  table_ref const& ref) {
  if (ddl_table const* table = find(ref)) {
    return table->columns;
  }
  return {};
}

std::vector<std::string> ddl_meta::primary_key(table_ref const& ref) {
  if (ddl_table const* table = find(ref)) {
    return table->primary_key;
  }
  return {};
}

std::vector<schema_meta::foreign_key_row> ddl_meta::foreign_keys(
  table_ref const& ref) {
  if (ddl_table const* table = find(ref)) {
    return table->foreign_keys;
  }
  return {};
}

std::vector<schema_meta::index_row> ddl_meta::indexes(
  table_ref const& ref) {
  if (ddl_table const* table = find(ref)) {
    return table->indexes;
  }
  return {};
}

}  // namespace uniorm::gen
