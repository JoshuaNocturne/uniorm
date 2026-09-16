#include "check.hpp"

void test_odbc_handles();
void test_odbc_error_is_backend_error();

int main() {
  test_odbc_handles();
  test_odbc_error_is_backend_error();

  int failures = uniorm::test::failure_count();
  if (failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
