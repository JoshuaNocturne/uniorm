#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <uniorm/backend/error.hpp>

#include "libpq/sql_text.hpp"

#include "check.hpp"

using namespace uniorm;

namespace {

std::pair<std::string, std::size_t> rewrite(std::string_view sql) {
  std::size_t count = 0;
  std::string out = libpq::rewrite_placeholders(sql, count);
  return {std::move(out), count};
}

}  // namespace

void test_libpq_placeholder_rewrite() {
  // Numbered in the order they appear, so the position the core binds at is
  // the one the server reads.
  auto const plain = rewrite("select a from t where b = ? and c > ?");
  CHECK(plain.second == 2);
  CHECK(plain.first == "select a from t where b = $1 and c > $2");

  auto const none = rewrite("select 1");
  CHECK(none.second == 0);
  CHECK(none.first == "select 1");

  // A ? that sits inside quoted text is part of the text.
  auto const literal = rewrite("select '?' from t where b = ?");
  CHECK(literal.second == 1);
  CHECK(literal.first == "select '?' from t where b = $1");

  // A doubled quote is one quote of the value and does not close the literal.
  auto const doubled = rewrite("select 'it''s ?' from t where b = ?");
  CHECK(doubled.second == 1);
  CHECK(doubled.first == "select 'it''s ?' from t where b = $1");

  auto const quoted_name = rewrite("select \"A?B\" from t where \"?\" = ?");
  CHECK(quoted_name.second == 1);
  CHECK(quoted_name.first == "select \"A?B\" from t where \"?\" = $1");

  // A dollar an identifier carries is copied, and a position already spelled
  // $n is not read as the start of a quoted body.
  auto const dollars = rewrite("select a$b, $1 + 1");
  CHECK(dollars.second == 0);
  CHECK(dollars.first == "select a$b, $1 + 1");
}

void test_libpq_placeholder_keeps_an_operator_mark() {
  // This server spells several of its own operators with a lone ?, and a marker
  // that ate them would take the operator's character for a parameter. Two
  // marks written together leave one of them to the server.
  auto const existence = rewrite("select body from t where meta ?? ?");
  CHECK(existence.second == 1);
  CHECK(existence.first == "select body from t where meta ? $1");

  // The keys-any and keys-all spellings keep their second half.
  auto const any = rewrite("select meta ??| ?, meta ??& ?");
  CHECK(any.second == 2);
  CHECK(any.first == "select meta ?| $1, meta ?& $2");

  auto const apart = rewrite("select ?? , ?");
  CHECK(apart.second == 1);
  CHECK(apart.first == "select ? , $1");

  // Three marks are a pair read left to right and then a marker.
  auto const three = rewrite("select ???");
  CHECK(three.second == 1);
  CHECK(three.first == "select ?$1");

  // Marks a construct carries are still that construct's own characters.
  auto const quoted = rewrite("select \"??\", 'a??b' , ?");
  CHECK(quoted.second == 1);
  CHECK(quoted.first == "select \"??\", 'a??b' , $1");
}

void test_libpq_placeholder_skips_comments() {
  // A line comment ends at the newline, which is kept so the next word stays
  // separated from the mark.
  auto const line = rewrite("select ? -- skip ?\nfrom t where b = ?");
  CHECK(line.second == 2);
  CHECK(line.first == "select $1 -- skip ?\nfrom t where b = $2");

  // These nest, so the inner close is not the outer one's.
  auto const nested = rewrite("select ? /* a /* ? */ b */ ?");
  CHECK(nested.second == 2);
  CHECK(nested.first == "select $1 /* a /* ? */ b */ $2");

  auto const trailing = rewrite("select a /* ? */ from t");
  CHECK(trailing.second == 0);
  CHECK(trailing.first == "select a /* ? */ from t");
}

void test_libpq_placeholder_skips_dollar_quotes() {
  auto const unnamed = rewrite("select $$ ? $$ where b = ?");
  CHECK(unnamed.second == 1);
  CHECK(unnamed.first == "select $$ ? $$ where b = $1");

  // A tagged body runs to its own tag, and a body of one tag may hold the
  // other spelling.
  auto const tagged =
    rewrite("create function f() returns int as $body$ select '?' $body$ "
            "language sql");
  CHECK(tagged.second == 0);
  CHECK(tagged.first == "create function f() returns int as $body$ select "
                        "'?' $body$ language sql");

  auto const inside = rewrite("select $tag$ $$ $tag$, ?");
  CHECK(inside.second == 1);
  CHECK(inside.first == "select $tag$ $$ $tag$, $1");
}

void test_libpq_placeholder_skips_escape_strings() {
  // The backslash takes the next character with it, so the first quote here
  // is the value's own and the second one ends the string.
  auto const escaped = rewrite("select E'\\'' , ?");
  CHECK(escaped.second == 1);
  CHECK(escaped.first == "select E'\\'' , $1");

  // An E leading a longer word belongs to that word, and the string after it
  // keeps the server's ordinary doubling rule.
  auto const word = rewrite("select name'E?' , ?");
  CHECK(word.second == 1);
  CHECK(word.first == "select name'E?' , $1");

  // Outside an escape string a backslash is one character of the value.
  auto const plain = rewrite("select 'a\\?', ?");
  CHECK(plain.second == 1);
  CHECK(plain.first == "select 'a\\?', $1");
}

void test_libpq_placeholder_rejects_unterminated() {
  // A statement that runs out inside a construct has no reading, and guessing
  // one would bind parameters the caller never wrote.
  std::size_t count = 0;
  CHECK_THROWS(libpq::rewrite_placeholders("select 'abc", count),
    backend::backend_error);
  CHECK_THROWS(libpq::rewrite_placeholders("select \"abc", count),
    backend::backend_error);
  CHECK_THROWS(libpq::rewrite_placeholders("select /* abc", count),
    backend::backend_error);
  CHECK_THROWS(libpq::rewrite_placeholders("select /* /* abc */", count),
    backend::backend_error);
  CHECK_THROWS(libpq::rewrite_placeholders("select $$ abc", count),
    backend::backend_error);
  CHECK_THROWS(libpq::rewrite_placeholders("select $tag$ abc", count),
    backend::backend_error);
  CHECK_THROWS(libpq::rewrite_placeholders("select E'abc\\", count),
    backend::backend_error);
}
