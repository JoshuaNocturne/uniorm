add_executable(uniorm_unit_tests
    unit/test_main.cpp
    unit/test_unicode.cpp
    unit/test_odbc_handles.cpp
    unit/test_pfr.cpp
    unit/test_row.cpp
    unit/test_params.cpp
    unit/test_expression.cpp
    unit/test_registry.cpp
)
target_link_libraries(uniorm_unit_tests PRIVATE uniorm::uniorm)
add_test(NAME unit_tests COMMAND uniorm_unit_tests)

add_executable(uniorm_integration_tests
    integration/test_integration.cpp
)
target_link_libraries(uniorm_integration_tests PRIVATE uniorm::uniorm ODBC::ODBC)
add_test(NAME integration_tests COMMAND uniorm_integration_tests)
set_tests_properties(integration_tests PROPERTIES SKIP_RETURN_CODE 77)
