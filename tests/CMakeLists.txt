add_executable(uniorm_unit_tests
    unit/test_main.cpp
    unit/test_unicode.cpp
    unit/test_pfr.cpp
    unit/test_row.cpp
    unit/test_params.cpp
    unit/test_expression.cpp
    unit/test_registry.cpp
    unit/test_backend_registry.cpp
)
# Deliberately links no ODBC: any driver-type leak into the public
# headers above the statement layer fails this target's compile.
target_link_libraries(uniorm_unit_tests PRIVATE uniorm::uniorm)
if(UNIORM_BACKEND_ODBC)
    target_compile_definitions(uniorm_unit_tests PRIVATE UNIORM_TEST_BACKEND_ODBC)
endif()
add_test(NAME unit_tests COMMAND uniorm_unit_tests)

if(UNIORM_BACKEND_ODBC)
    add_executable(uniorm_odbc_unit_tests
        unit/test_odbc_main.cpp
        unit/test_odbc_handles.cpp
    )
    target_link_libraries(uniorm_odbc_unit_tests PRIVATE uniorm::uniorm ODBC::ODBC)
    if(UNIORM_BUILD_TOOLS)
        target_sources(uniorm_odbc_unit_tests PRIVATE
            unit/test_gen_config.cpp
            unit/test_gen_output.cpp
        )
        target_link_libraries(uniorm_odbc_unit_tests PRIVATE uniorm_gen_core)
        target_compile_definitions(uniorm_odbc_unit_tests PRIVATE UNIORM_TEST_GEN)
    endif()
    add_test(NAME odbc_unit_tests COMMAND uniorm_odbc_unit_tests)

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

    if(UNIORM_BUILD_TOOLS)
        add_executable(uniorm_gen_e2e_tests
            integration/test_gen_e2e.cpp
        )
        target_link_libraries(uniorm_gen_e2e_tests PRIVATE uniorm::uniorm ODBC::ODBC)
        add_dependencies(uniorm_gen_e2e_tests uniorm-gen)
        target_compile_definitions(uniorm_gen_e2e_tests PRIVATE
            UNIORM_GEN_PATH="$<TARGET_FILE:uniorm-gen>"
            UNIORM_GEN_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/gen_out"
            UNIORM_GEN_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/integration/golden/gen_it_schema.hpp"
        )
        add_test(NAME gen_e2e_tests COMMAND uniorm_gen_e2e_tests)
        set_tests_properties(gen_e2e_tests PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
