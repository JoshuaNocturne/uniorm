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
if(UNIORM_BUILD_TOOLS)
    target_sources(uniorm_unit_tests PRIVATE
        unit/test_gen_config.cpp
        unit/test_gen_output.cpp
    )
    target_link_libraries(uniorm_unit_tests PRIVATE uniorm_gen_core)
    target_compile_definitions(uniorm_unit_tests PRIVATE UNIORM_TEST_GEN)
endif()
add_test(NAME unit_tests COMMAND uniorm_unit_tests)

add_executable(uniorm_integration_tests
    integration/test_integration.cpp
)
target_link_libraries(uniorm_integration_tests PRIVATE uniorm::uniorm ODBC::ODBC)
add_test(NAME integration_tests COMMAND uniorm_integration_tests)
set_tests_properties(integration_tests PROPERTIES SKIP_RETURN_CODE 77)

add_executable(uniorm_perf_tests
    perf/test_perf.cpp
)
target_link_libraries(uniorm_perf_tests PRIVATE uniorm::uniorm ODBC::ODBC)
add_test(NAME perf_tests COMMAND uniorm_perf_tests)
set_tests_properties(perf_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS perf)
