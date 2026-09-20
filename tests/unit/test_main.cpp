#include "check.hpp"

void test_pfr();
void test_row();
void test_decimal();
void test_params();
void test_converter();
void test_expression();
void test_registry();
void test_orm_crud_helpers();
void test_orm_validate();
void test_backend_registry();
void test_pool();
#ifdef UNIORM_TEST_GEN
void test_gen_config();
void test_gen_output();
#endif

int main() {
  test_pfr();
  test_row();
  test_decimal();
  test_params();
  test_converter();
  test_expression();
  test_registry();
  test_orm_crud_helpers();
  test_orm_validate();
  test_backend_registry();
  test_pool();
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
