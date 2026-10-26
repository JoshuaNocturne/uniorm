#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <uniorm/backend/error.hpp>

#include "oracle_oci/sql_text.hpp"

#include "check.hpp"

using namespace uniorm;

namespace {

std::pair<std::string, std::size_t> rewrite(std::string_view sql) {
  std::size_t count = 0;
  std::string out = oracle_oci::rewrite_placeholders(sql, count);
  return {std::move(out), count};
}

}  // namespace

void test_oracle_oci_placeholder_rewrite() {
  // Numbered in the order they appear, so OCIBindByPos counted from the
  // caller's parameter list lands on the same placeholder.
  auto const plain = rewrite("select a from t where b = ? and c > ?");
  CHECK(plain.second == 2);
  CHECK(plain.first == "select a from t where b = :1 and c > :2");

  auto const none = rewrite("select 1 from dual");
  CHECK(none.second == 0);
  CHECK(none.first == "select 1 from dual");

  // A ? that sits inside quoted text is part of the text.
  auto const literal = rewrite("select '?' from t where b = ?");
  CHECK(literal.second == 1);
  CHECK(literal.first == "select '?' from t where b = :1");

  // A doubled quote is one quote of the value and does not close the literal.
  auto const doubled = rewrite("select 'it''s ?' from t where b = ?");
  CHECK(doubled.second == 1);
  CHECK(doubled.first == "select 'it''s ?' from t where b = :1");

  auto const quoted_name = rewrite("select \"A?B\" from t where \"?\" = ?");
  CHECK(quoted_name.second == 1);
  CHECK(quoted_name.first == "select \"A?B\" from t where \"?\" = :1");

  // Oracle's alternate quoting: a bracket opener demands its own closer.
  auto const bracketed = rewrite("insert into t values (q'[?]', ?)");
  CHECK(bracketed.second == 1);
  CHECK(bracketed.first == "insert into t values (q'[?]', :1)");

  // Any other character closes with itself, in either case.
  auto const self_closing = rewrite("insert into t values (q'?yes?', ?)");
  CHECK(self_closing.second == 1);
  CHECK(self_closing.first == "insert into t values (q'?yes?', :1)");

  auto const upper = rewrite("select Q'{?}' from t where b = ?");
  CHECK(upper.second == 1);
  CHECK(upper.first == "select Q'{?}' from t where b = :1");

  // Comments carry no placeholders either, and keep their bytes so the next
  // token stays separated the way the caller wrote it.
  auto const line = rewrite("select ? -- b = ?\nfrom t where c = ?");
  CHECK(line.second == 2);
  CHECK(line.first == "select :1 -- b = ?\nfrom t where c = :2");

  auto const block = rewrite("select /* ? */ ? from t");
  CHECK(block.second == 1);
  CHECK(block.first == "select /* ? */ :1 from t");
}

void test_oracle_oci_placeholder_rejects_unterminated() {
  // Text ending inside a quote or a block comment is broken SQL. Binding
  // nothing and letting the server guess would report it as bad syntax at the
  // wrong offset.
  CHECK_THROWS(rewrite("select 'abc"), backend::backend_error);
  CHECK_THROWS(rewrite("select \"abc"), backend::backend_error);
  CHECK_THROWS(rewrite("select /* abc"), backend::backend_error);
  CHECK_THROWS(rewrite("select q'[abc"), backend::backend_error);
  CHECK_THROWS(rewrite("select q'"), backend::backend_error);
}

void test_oracle_oci_leads_with_query() {
  CHECK(oracle_oci::leads_with_query("select 1 from dual"));
  CHECK(oracle_oci::leads_with_query("  SELECT * FROM t WHERE id = ?"));
  CHECK(oracle_oci::leads_with_query(
    "with x as (select 1 from dual) select * from x"));
  CHECK(oracle_oci::leads_with_query(
    "WitH x AS (SELECT 1 FROM dual) SELECT 1"));
  CHECK(oracle_oci::leads_with_query("-- a note\nselect 1 from dual"));
  CHECK(oracle_oci::leads_with_query(
    "/* a note */ with q as (select 1) select 1"));

  CHECK(!oracle_oci::leads_with_query("insert into t values (1)"));
  CHECK(!oracle_oci::leads_with_query("update t set a = 1"));
  CHECK(!oracle_oci::leads_with_query("begin some_procedure; end;"));
  CHECK(!oracle_oci::leads_with_query("create table t (a int)"));
  CHECK(!oracle_oci::leads_with_query(""));
  CHECK(!oracle_oci::leads_with_query("   "));
  // A keyword has to be the whole first word, not merely its start.
  CHECK(!oracle_oci::leads_with_query("selected_from"));
}
