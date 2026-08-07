#include "check.hpp"

void test_unicode();
void test_odbc_handles();
void test_pfr();
void test_row();
void test_params();
void test_expression();
void test_registry();

int main() {
  test_unicode();
  test_odbc_handles();
  test_pfr();
  test_row();
  test_params();
  test_expression();
  test_registry();

  int failures = uniorm::test::failure_count();
  if (failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
