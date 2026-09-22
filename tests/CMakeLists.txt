add_executable(uniorm_unit_tests
    unit/test_main.cpp
    unit/test_pfr.cpp
    unit/test_row.cpp
    unit/test_decimal.cpp
    unit/test_params.cpp
    unit/test_converter.cpp
    unit/test_expression.cpp
    unit/test_registry.cpp
    unit/test_orm_crud_helpers.cpp
    unit/test_orm_validate.cpp
    unit/test_identifier_resolution.cpp
    unit/test_backend_registry.cpp
    unit/test_pool.cpp
)
# Deliberately links no ODBC: any driver-type leak into the public
# headers above the statement layer fails this target's compile.
target_link_libraries(uniorm_unit_tests PRIVATE uniorm::uniorm)
# test_orm_crud_helpers exercises a private header that sits beside its
# source; src/ is on the include path for no other reason.
target_include_directories(uniorm_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
if(UNIORM_BUILD_TOOLS)
    # The generator's own logic names no driver any more, so it is tested in
    # the target that links none.
    target_sources(uniorm_unit_tests PRIVATE
        unit/test_gen_config.cpp
        unit/test_gen_output.cpp
        unit/test_gen_reader.cpp
    )
    target_link_libraries(uniorm_unit_tests PRIVATE uniorm_gen_core)
    target_compile_definitions(uniorm_unit_tests PRIVATE UNIORM_TEST_GEN)
endif()
add_test(NAME unit_tests COMMAND uniorm_unit_tests)

if(UNIORM_BACKEND_ODBC)
    # The white-box targets below still link uniorm::odbc so a driver-side
    # symbol is reachable at build time. Under the plugin shape (design.md
    # §5.1) that linkage does not register "odbc" -- registration goes through
    # uniorm_plugin_register, which the core's loader calls after dlopen. In
    # the build tree the loader's baked UNIORM_PLUGIN_DIR points at the
    # install prefix and does not exist yet; UNIORM_PLUGIN_PATH redirects it
    # at the .so CMake just built here.
    set(UNIORM_BUILD_TREE_PLUGIN_DIR "$<TARGET_FILE_DIR:uniorm_odbc>")

    add_executable(uniorm_odbc_unit_tests
        unit/test_odbc_main.cpp
        unit/test_odbc_handles.cpp
        unit/test_odbc_catalog.cpp
    )
    # White-box test of the backend's own layer: link uniorm::odbc for the
    # symbols under test and ODBC::ODBC for the SQL* headers the private
    # odbc/*.hpp files include.
    target_link_libraries(uniorm_odbc_unit_tests
        PRIVATE uniorm::odbc ODBC::ODBC)
    target_include_directories(uniorm_odbc_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME odbc_unit_tests COMMAND uniorm_odbc_unit_tests)

    add_executable(uniorm_integration_tests
        integration/test_integration.cpp
    )
    target_link_libraries(uniorm_integration_tests PRIVATE uniorm::odbc)
    add_test(NAME integration_tests COMMAND uniorm_integration_tests)
    set_tests_properties(integration_tests PROPERTIES
        SKIP_RETURN_CODE 77
        ENVIRONMENT "UNIORM_PLUGIN_PATH=${UNIORM_BUILD_TREE_PLUGIN_DIR}")

    add_executable(uniorm_perf_tests
        perf/test_perf.cpp
    )
    # The perf case opens a driver handle directly for its timing loop.
    target_link_libraries(uniorm_perf_tests PRIVATE uniorm::odbc ODBC::ODBC)
    add_test(NAME perf_tests COMMAND uniorm_perf_tests)
    set_tests_properties(perf_tests PROPERTIES
        SKIP_RETURN_CODE 77 LABELS perf
        ENVIRONMENT "UNIORM_PLUGIN_PATH=${UNIORM_BUILD_TREE_PLUGIN_DIR}")

    if(UNIORM_BUILD_TOOLS)
        add_executable(uniorm_gen_e2e_tests
            integration/test_gen_e2e.cpp
        )
        target_link_libraries(uniorm_gen_e2e_tests PRIVATE uniorm::odbc)
        add_dependencies(uniorm_gen_e2e_tests uniorm-gen)
        target_compile_definitions(uniorm_gen_e2e_tests PRIVATE
            UNIORM_GEN_PATH="$<TARGET_FILE:uniorm-gen>"
            UNIORM_GEN_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/gen_out"
            UNIORM_GEN_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/integration/golden/gen_it_schema.hpp"
            UNIORM_GEN_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/integration/golden/gen_it.toml"
        )
        add_test(NAME gen_e2e_tests COMMAND uniorm_gen_e2e_tests)
        set_tests_properties(gen_e2e_tests PROPERTIES
            SKIP_RETURN_CODE 77
            ENVIRONMENT "UNIORM_PLUGIN_PATH=${UNIORM_BUILD_TREE_PLUGIN_DIR}")
    endif()
endif()

# Install rules exist only for a top-level build, and this check configures and
# compiles an outside project, so a cross-compiled build cannot run it.
# GNUInstallDirs is included here because the root file includes it later, in
# the install section that follows add_subdirectory(tests).
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    include(GNUInstallDirs)
    configure_file(install_smoke.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/install_smoke.cmake @ONLY)
    add_test(NAME install_smoke
        COMMAND ${CMAKE_COMMAND} -P
                ${CMAKE_CURRENT_BINARY_DIR}/install_smoke.cmake)
endif()
