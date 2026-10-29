#include "check.hpp"

void test_libpq_placeholder_rewrite();
void test_libpq_placeholder_keeps_an_operator_mark();
void test_libpq_placeholder_skips_comments();
void test_libpq_placeholder_skips_dollar_quotes();
void test_libpq_placeholder_skips_escape_strings();
void test_libpq_placeholder_rejects_unterminated();
void test_libpq_declared_shape();
void test_libpq_sql_type_from_native();
void test_libpq_display_width();
void test_libpq_text_shape();
void test_libpq_text_of_timestamp();
void test_libpq_conninfo_keeps_a_written_target();
void test_libpq_conninfo_from_pieces();
void test_libpq_conninfo_quotes_a_value_that_needs_it();
void test_libpq_open_needs_a_target();
void test_libpq_connect_failure_keeps_the_password();
void test_libpq_error_is_backend_error();
void test_libpq_error_reports_an_unnamed_failure();
void test_libpq_statement_declines_a_batch_writer();
void test_libpq_statement_binds_nothing_before_a_prepare();
void test_libpq_statement_refuses_a_column_before_a_result();

int main() {
  test_libpq_placeholder_rewrite();
  test_libpq_placeholder_keeps_an_operator_mark();
  test_libpq_placeholder_skips_comments();
  test_libpq_placeholder_skips_dollar_quotes();
  test_libpq_placeholder_skips_escape_strings();
  test_libpq_placeholder_rejects_unterminated();
  test_libpq_declared_shape();
  test_libpq_sql_type_from_native();
  test_libpq_display_width();
  test_libpq_text_shape();
  test_libpq_text_of_timestamp();
  test_libpq_conninfo_keeps_a_written_target();
  test_libpq_conninfo_from_pieces();
  test_libpq_conninfo_quotes_a_value_that_needs_it();
  test_libpq_open_needs_a_target();
  test_libpq_connect_failure_keeps_the_password();
  test_libpq_error_is_backend_error();
  test_libpq_error_reports_an_unnamed_failure();
  test_libpq_statement_declines_a_batch_writer();
  test_libpq_statement_binds_nothing_before_a_prepare();
  test_libpq_statement_refuses_a_column_before_a_result();

  int failures = uniorm::test::failure_count();
  if (failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
