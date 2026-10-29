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
    unit/test_binding_order.cpp
    unit/test_backend_registry.cpp
    unit/test_pool.cpp
    unit/test_async_task.cpp
    unit/test_async_orm.cpp
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
        unit/test_gen_ddl.cpp
        unit/test_gen_output.cpp
        unit/test_gen_reader.cpp
    )
    target_link_libraries(uniorm_unit_tests PRIVATE uniorm_gen_core)
    target_compile_definitions(uniorm_unit_tests PRIVATE UNIORM_TEST_GEN)
endif()
add_test(NAME unit_tests COMMAND uniorm_unit_tests)
# Windows loads DLLs from the executable's directory; the build tree puts
# uniorm.dll beside the core target, not the test, so copy it over.
uniorm_copy_runtime_dlls(uniorm_unit_tests)

# Each installed header has to compile on its own. The umbrella is a
# convenience, so a consumer that names one header directly cannot be
# expected to have included another one first; and the installed-tree check
# in install_smoke compares file lists, not compilations, so a header that
# only compiles behind a neighbour's include passes it. One TU per header,
# compiled and never linked: a missing include becomes a build failure here
# rather than a surprise in someone else's project.
file(GLOB_RECURSE uniorm_public_headers RELATIVE ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/uniorm/*.hpp)
set(uniorm_header_check_sources "")
foreach(header IN LISTS uniorm_public_headers)
    string(REGEX REPLACE "[/:.]" "_" slug ${header})
    set(check_source ${CMAKE_CURRENT_BINARY_DIR}/header_checks/${slug}.cpp)
    # file(GENERATE) writes only when the content changes, so reconfiguring
    # does not recompile every one of these TUs.
    file(GENERATE OUTPUT ${check_source} CONTENT "#include <${header}>\n")
    list(APPEND uniorm_header_check_sources ${check_source})
endforeach()
add_library(uniorm_header_check OBJECT ${uniorm_header_check_sources})
target_link_libraries(uniorm_header_check PRIVATE uniorm::uniorm)

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
    # Reaching the backend's private headers by name ("odbc/error.hpp") is
    # exactly what the plugin-shape split makes the source of truth for
    # visibility, so -I here is the whole surface the tests can peek at.
    target_include_directories(uniorm_odbc_unit_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/backends ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME odbc_unit_tests COMMAND uniorm_odbc_unit_tests)
    uniorm_copy_runtime_dlls(uniorm_odbc_unit_tests)

    add_executable(uniorm_integration_tests
        integration/test_integration.cpp
    )
    target_link_libraries(uniorm_integration_tests PRIVATE uniorm::odbc)
    # The integration case folds the names it reads back, so it names the
    # core's private folding header rather than a second copy of the rule.
    target_include_directories(uniorm_integration_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME integration_tests COMMAND uniorm_integration_tests)
    set_tests_properties(integration_tests PROPERTIES
        SKIP_RETURN_CODE 77
        ENVIRONMENT "UNIORM_PLUGIN_PATH=${UNIORM_BUILD_TREE_PLUGIN_DIR}")
    uniorm_copy_runtime_dlls(uniorm_integration_tests)

    add_executable(uniorm_perf_tests
        perf/test_perf.cpp
    )
    if(WIN32)
        target_compile_options(uniorm_perf_tests PRIVATE
            /utf-8)
    endif()
    # The perf case opens a driver handle directly for its timing loop.
    target_link_libraries(uniorm_perf_tests PRIVATE uniorm::odbc ODBC::ODBC)
    add_test(NAME perf_tests COMMAND uniorm_perf_tests)
    set_tests_properties(perf_tests PROPERTIES
        SKIP_RETURN_CODE 77 LABELS perf
        ENVIRONMENT "UNIORM_PLUGIN_PATH=${UNIORM_BUILD_TREE_PLUGIN_DIR}")
    uniorm_copy_runtime_dlls(uniorm_perf_tests)

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
            UNIORM_GEN_SQL="${CMAKE_CURRENT_SOURCE_DIR}/integration/golden/gen_it.sql"
        )
        add_test(NAME gen_e2e_tests COMMAND uniorm_gen_e2e_tests)
        set_tests_properties(gen_e2e_tests PROPERTIES
            SKIP_RETURN_CODE 77
            ENVIRONMENT "UNIORM_PLUGIN_PATH=${UNIORM_BUILD_TREE_PLUGIN_DIR}")
        uniorm_copy_runtime_dlls(uniorm_gen_e2e_tests)
    endif()
endif()

if(UNIORM_BACKEND_LIBPQ)
    # The same white-box shape as the ODBC targets above: the symbols under
    # test are the plugin's own, and PostgreSQL::libpq is named here as well
    # because the backend links it privately -- the tests read libpq-fe.h
    # through the backend's private headers.
    add_executable(uniorm_libpq_unit_tests
        unit/test_libpq_main.cpp
        unit/test_libpq_sql_text.cpp
        unit/test_libpq_types.cpp
        unit/test_libpq_connection.cpp
        unit/test_libpq_statement.cpp
    )
    target_link_libraries(uniorm_libpq_unit_tests
        PRIVATE uniorm::libpq PostgreSQL::libpq)
    target_include_directories(uniorm_libpq_unit_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/backends)
    add_test(NAME libpq_unit_tests COMMAND uniorm_libpq_unit_tests)
    uniorm_copy_runtime_dlls(uniorm_libpq_unit_tests)

    # The same suite over the libpq plugin: one source, and the leg picks its
    # server from UNIORM_IT_BACKEND. It links only the core because the plugin
    # is dlopened by scheme.
    add_executable(uniorm_integration_postgresql_tests
        integration/test_integration.cpp
    )
    target_link_libraries(uniorm_integration_postgresql_tests
        PRIVATE uniorm::uniorm)
    target_include_directories(uniorm_integration_postgresql_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src)
    add_dependencies(uniorm_integration_postgresql_tests uniorm_libpq)
    add_test(NAME integration_tests_postgresql
        COMMAND uniorm_integration_postgresql_tests)
    # ENVIRONMENT takes one ;-separated list of settings.
    set(libpq_plugin_dir "$<TARGET_FILE_DIR:uniorm_libpq>")
    string(JOIN ";" libpq_integration_env
        "UNIORM_IT_BACKEND=postgresql" "UNIORM_PLUGIN_PATH=${libpq_plugin_dir}")
    set_tests_properties(integration_tests_postgresql PROPERTIES
        SKIP_RETURN_CODE 77
        ENVIRONMENT "${libpq_integration_env}")
    uniorm_copy_runtime_dlls(uniorm_integration_postgresql_tests)
endif()

if(UNIORM_BACKEND_ORACLE_OCI)
    # The same white-box shape as the ODBC targets above: the symbols under
    # test are the plugin's own, and Oracle::OCI is named here as well because
    # the backend links it privately -- the tests read the OCI headers through
    # the backend's private ones.
    add_executable(uniorm_oracle_oci_unit_tests
        unit/test_oracle_oci_main.cpp
        unit/test_oracle_oci_sql_text.cpp
        unit/test_oracle_oci_types.cpp
        unit/test_oracle_oci_connection.cpp
    )
    target_link_libraries(uniorm_oracle_oci_unit_tests
        PRIVATE uniorm::oracle_oci Oracle::OCI)
    target_include_directories(uniorm_oracle_oci_unit_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/backends)
    add_test(NAME oracle_oci_unit_tests COMMAND uniorm_oracle_oci_unit_tests)
    uniorm_copy_runtime_dlls(uniorm_oracle_oci_unit_tests)

    # The same suite over the Oracle plugin: one source, and the leg picks its
    # server from UNIORM_IT_BACKEND. It links only the core because the plugin
    # is dlopened by scheme, and linking it would put oci.h on this target's
    # include path.
    add_executable(uniorm_integration_oracle_tests
        integration/test_integration.cpp
    )
    target_link_libraries(uniorm_integration_oracle_tests
        PRIVATE uniorm::uniorm)
    target_include_directories(uniorm_integration_oracle_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src)
    add_dependencies(uniorm_integration_oracle_tests uniorm_oracle_oci)
    add_test(NAME integration_tests_oracle
        COMMAND uniorm_integration_oracle_tests)
    # ENVIRONMENT takes one ;-separated list of settings.
    set(oracle_plugin_dir "$<TARGET_FILE_DIR:uniorm_oracle_oci>")
    string(JOIN ";" oracle_integration_env
        "UNIORM_IT_BACKEND=oracle" "UNIORM_PLUGIN_PATH=${oracle_plugin_dir}")
    set_tests_properties(integration_tests_oracle PROPERTIES
        SKIP_RETURN_CODE 77
        ENVIRONMENT "${oracle_integration_env}")
    uniorm_copy_runtime_dlls(uniorm_integration_oracle_tests)
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
