#include "check.hpp"

void test_oracle_oci_placeholder_rewrite();
void test_oracle_oci_placeholder_rejects_unterminated();
void test_oracle_oci_leads_with_query();
void test_oracle_oci_number_shape();
void test_oracle_oci_sql_type_from_native();
void test_oracle_oci_wide_columns();
void test_oracle_oci_logon_needs_a_target();
void test_oracle_oci_logon_splits_credentials();
void test_oracle_oci_logon_cuts_at_the_last_marker();
void test_oracle_oci_logon_without_credentials();
void test_oracle_oci_error_is_backend_error();
void test_oracle_oci_error_reports_a_tolerated_code();

int main() {
  test_oracle_oci_placeholder_rewrite();
  test_oracle_oci_placeholder_rejects_unterminated();
  test_oracle_oci_leads_with_query();
  test_oracle_oci_number_shape();
  test_oracle_oci_sql_type_from_native();
  test_oracle_oci_wide_columns();
  test_oracle_oci_logon_needs_a_target();
  test_oracle_oci_logon_splits_credentials();
  test_oracle_oci_logon_cuts_at_the_last_marker();
  test_oracle_oci_logon_without_credentials();
  test_oracle_oci_error_is_backend_error();
  test_oracle_oci_error_reports_a_tolerated_code();

  int failures = uniorm::test::failure_count();
  if (failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
