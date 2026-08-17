#include "check.hpp"

void test_odbc_handles();
#ifdef UNIORM_TEST_GEN
void test_gen_config();
void test_gen_output();
#endif

int main() {
  test_odbc_handles();
#ifdef UNIORM_TEST_GEN
  test_gen_config();
  test_gen_output();
#endif

  int failures = uniorm::test::failure_count();
  if (failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
