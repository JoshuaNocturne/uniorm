cmake_minimum_required(VERSION 3.20)
project(uniorm VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Build mode switch: defaults to Debug; pass -DCMAKE_BUILD_TYPE=Release etc.
# Debug  -> -g (CMake default flags); Release -> -O3 -DNDEBUG (CMake default flags)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
message(STATUS "uniorm build type: ${CMAKE_BUILD_TYPE}")

option(UNIORM_BUILD_TESTS "Build uniorm tests" ON)
option(UNIORM_BUILD_TOOLS "Build uniorm tools (uniorm-gen)" ON)
option(UNIORM_BACKEND_ODBC "Build the ODBC backend" ON)

if(UNIORM_BACKEND_ODBC)
    find_package(ODBC REQUIRED)
endif()
find_package(Threads REQUIRED)

add_library(uniorm SHARED
    src/types.cpp
    src/decimal.cpp
    src/row.cpp
    src/result_set.cpp
    src/connection.cpp
    src/dialect.cpp
    src/transaction.cpp
    src/pool.cpp
    src/backend/error.cpp
    src/backend/registry.cpp
    src/builder/expression.cpp
    src/builder/builder.cpp
    src/mapping/registry.cpp
    src/orm.cpp
)
add_library(uniorm::uniorm ALIAS uniorm)

# The public headers need C++20 themselves (concepts, remove_cvref_t), and the
# global CMAKE_CXX_STANDARD above does not cross into a consumer's flags.
target_compile_features(uniorm PUBLIC cxx_std_20)
target_include_directories(uniorm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# Private headers sit beside their sources in src/ and are included by relative
# name, so src/ is on no include path; consumers cannot reach them.
# ODBC is PRIVATE: the backend contract keeps driver types out of the
# installed headers, so consumers never see them.
target_link_libraries(uniorm PRIVATE Threads::Threads)
if(UNIORM_BACKEND_ODBC)
    target_sources(uniorm PRIVATE
        src/odbc/error.cpp
        src/odbc/environment.cpp
        src/odbc/connection.cpp
        src/odbc/statement.cpp
        src/odbc/backend.cpp
    )
    target_link_libraries(uniorm PRIVATE ODBC::ODBC)
endif()
target_compile_definitions(uniorm PRIVATE UNIORM_BUILDING_DLL)

if(UNIORM_BUILD_TOOLS AND NOT UNIORM_BACKEND_ODBC)
    message(FATAL_ERROR
        "UNIORM_BUILD_TOOLS requires UNIORM_BACKEND_ODBC: "
        "uniorm-gen reads live schema metadata through ODBC")
endif()

if(UNIORM_BUILD_TOOLS)
    add_subdirectory(tools/uniorm-gen)
endif()

if(UNIORM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Installation ---
# Guarded so that a project pulling uniorm in with add_subdirectory or
# FetchContent links uniorm::uniorm without also inheriting our install rules.
if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # A pre-1.0 library has no ABI promise to keep, and this one has already
    # broken silently once (adding a member to connection changed its size).
    # Version the soname by major.minor so a consumer's stale binary fails at
    # link time instead of at run time, and match that in the package version.
    set_target_properties(uniorm PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    )

    # RUNTIME covers the Windows DLL, ARCHIVE its import library (consumers
    # need it: UNIORM_API expands to dllimport outside the building target),
    # LIBRARY the shared object and its symlinks on ELF platforms.
    install(TARGETS uniorm
        EXPORT uniormTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    # Everything under include/ is public by construction: the private headers
    # sit beside their sources in src/, and an installed header that reached one
    # would fail to compile for a consumer that only sees include/.
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/uniorm
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp"
    )

    set(UNIORM_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/uniorm)

    install(EXPORT uniormTargets
        FILE uniormTargets.cmake
        NAMESPACE uniorm::
        DESTINATION ${UNIORM_CMAKE_INSTALL_DIR}
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uniormConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/uniormConfig.cmake
        INSTALL_DESTINATION ${UNIORM_CMAKE_INSTALL_DIR}
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/uniormConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/uniormConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/uniormConfigVersion.cmake
        DESTINATION ${UNIORM_CMAKE_INSTALL_DIR}
    )

    # A developer-time tool rather than part of the consumption surface, so it
    # ships only in builds that asked for it (which requires the ODBC backend).
    if(UNIORM_BUILD_TOOLS)
        # The binary's NEEDED entry names the versioned soname, and the build
        # tree leaves an absolute RPATH behind: without a prefix-relative load
        # path the installed copy only runs on the machine that built it. RPATH
        # is ELF-only: elsewhere the DLL already lands next to the exe.
        if(UNIX AND NOT APPLE AND NOT CYGWIN)
            set_target_properties(uniorm-gen PROPERTIES
                INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
            )
        endif()
        install(TARGETS uniorm-gen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()
