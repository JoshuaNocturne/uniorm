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
void test_identifier_resolution();
void test_backend_registry();
void test_pool();
#ifdef UNIORM_TEST_GEN
void test_gen_config();
void test_gen_output();
void test_gen_reader();
#endif

int main() {
  std::printf("Starting unit tests...\n");
  std::fflush(stdout);
  std::printf("Running test_pfr...\n");
  std::fflush(stdout);
  test_pfr();
  std::printf("Running test_row...\n");
  std::fflush(stdout);
  test_row();
  std::printf("Running test_decimal...\n");
  std::fflush(stdout);
  test_decimal();
  std::printf("Running test_params...\n");
  std::fflush(stdout);
  test_params();
  std::printf("Running test_converter...\n");
  std::fflush(stdout);
  test_converter();
  std::printf("Running test_expression...\n");
  std::fflush(stdout);
  test_expression();
  std::printf("Running test_registry...\n");
  std::fflush(stdout);
  test_registry();
  std::printf("Running test_orm_crud_helpers...\n");
  std::fflush(stdout);
  test_orm_crud_helpers();
  std::printf("Running test_orm_validate...\n");
  std::fflush(stdout);
  test_orm_validate();
  std::printf("Running test_identifier_resolution...\n");
  std::fflush(stdout);
  test_identifier_resolution();
  std::printf("Running test_backend_registry...\n");
  std::fflush(stdout);
  test_backend_registry();
  std::printf("Running test_pool...\n");
  std::fflush(stdout);
  test_pool();
#ifdef UNIORM_TEST_GEN
  test_gen_config();
  test_gen_output();
  test_gen_reader();
#endif

  int failures = uniorm::test::failure_count();
  if (failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
