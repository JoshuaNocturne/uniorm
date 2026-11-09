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
  return { std::move(out), count };
}

// What a batch of `rows` would be sent as: the rewrite, its fold, and the fold
// written out. An empty text comes back where the statement has no row group
// to repeat.
std::string folded(std::string_view sql, std::size_t rows) {
  auto const [rewritten, count] = rewrite(sql);
  return libpq::expand_values_fold(
    libpq::find_values_fold(rewritten, count), rows);
}

std::size_t fold_columns(std::string_view sql) {
  auto const [rewritten, count] = rewrite(sql);
  return libpq::find_values_fold(rewritten, count).columns;
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
  CHECK_THROWS(
    libpq::rewrite_placeholders("select 'abc", count), backend::backend_error);
  CHECK_THROWS(
    libpq::rewrite_placeholders("select \"abc", count), backend::backend_error);
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

void test_libpq_fold_repeats_a_row_group() {
  // One row's group, written as many times as the batch has rows, with the
  // parameters numbered on from the last group's.
  CHECK(folded("insert into t (a, b) values (?, ?)", 3) ==
        "insert into t (a, b) values ($1, $2), ($3, $4), ($5, $6)");

  // The column list is not needed, and neither is the space before the group.
  CHECK(folded("insert into t values (?, ?)", 2) ==
        "insert into t values ($1, $2), ($3, $4)");
  CHECK(folded("INSERT INTO t VALUES(?,?)", 2) ==
        "INSERT INTO t VALUES($1, $2), ($3, $4)");

  // Whatever the statement carries after its group belongs to the statement,
  // not to a row, so it stays where it was written.
  CHECK(folded("INSERT INTO t (a) VALUES (?) ON CONFLICT (a) DO NOTHING", 2) ==
        "INSERT INTO t (a) VALUES ($1), ($2) ON CONFLICT (a) DO NOTHING");
  CHECK(folded("insert into t (a) values (?) returning id", 2) ==
        "insert into t (a) values ($1), ($2) returning id");

  // One row is a group of one, and no rows is nothing to send.
  CHECK(folded("insert into t (a) values (?)", 1) ==
        "insert into t (a) values ($1)");
  CHECK(folded("insert into t (a) values (?)", 0).empty());

  CHECK(fold_columns("insert into t (a, b, c) values (?, ?, ?)") == 3);
}

void test_libpq_fold_declines_what_is_not_one_row() {
  // An UPDATE and a DELETE bind their values to a predicate, not to a row.
  CHECK(folded("update t set a = ? where b = ?", 3).empty());
  CHECK(folded("delete from t where a = ?", 3).empty());

  // A select that carries a VALUES of its own writes no row of the caller's,
  // however it is spelled.
  CHECK(folded("select * from (values (?), (?)) v", 2).empty());
  CHECK(folded("with x as (select ?) insert into t (a) values (?)", 2).empty());

  // Two groups is a text that already writes more than the row it was bound
  // for, and the fold has no way to say where the caller's rows end.
  CHECK(folded("insert into t (a, b) values (?, ?), (?, ?)", 2).empty());
  CHECK(folded("insert into t (a) values (?) , (?)", 2).empty());

  // A binding outside the group is one no row owns.
  CHECK(folded("insert into t (a, b) values (?, ?) "
               "on conflict (a) do update set b = ?",
    2)
      .empty());

  // An entry that is not a parameter would be repeated with every row, which
  // is a value the caller never named.
  CHECK(folded("insert into t (a, b) values (?, now())", 2).empty());
  CHECK(folded("insert into t (a) values (coalesce(?, 0))", 2).empty());
  CHECK(folded("insert into t (a) values (1)", 2).empty());
  CHECK(folded("insert into t default values", 2).empty());

  // A position written by number is indistinguishable, in the rewritten text,
  // from one the rewrite made; the count the rewrite declared is what says the
  // group holds only the caller's own markers.
  auto const numbered = rewrite("insert into t (a, b) values ($1, ?)");
  CHECK(numbered.second == 1);
  CHECK(libpq::find_values_fold(numbered.first, numbered.second).columns == 0);
}

void test_libpq_fold_skips_what_a_statement_hides() {
  // A quoted name is not the keyword, and does not hide the real one.
  CHECK(folded("insert into \"values\" (a) values (?)", 2) ==
        "insert into \"values\" (a) values ($1), ($2)");

  // A VALUES written inside a comment or a literal is that construct's own
  // text.
  CHECK(folded("insert into t -- values (?, ?)\n (a) values (?)", 2) ==
        "insert into t -- values (?, ?)\n (a) values ($1), ($2)");
  CHECK(folded("insert into t (a) values ('values (?)') ", 2).empty());
  CHECK(folded("insert into t (a) values ('values (?)'), (?)", 2).empty());
}
