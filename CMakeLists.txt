cmake_minimum_required(VERSION 3.20)
project(uniorm VERSION 0.1.0 LANGUAGES CXX)

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

set(UNIORM_DECIMAL_DEFAULT "string" CACHE STRING "Default C++ mapping for DECIMAL/NUMERIC columns")
set_property(CACHE UNIORM_DECIMAL_DEFAULT PROPERTY STRINGS string double)
if(NOT UNIORM_DECIMAL_DEFAULT MATCHES "^(string|double)$")
    message(FATAL_ERROR "UNIORM_DECIMAL_DEFAULT must be 'string' or 'double'")
endif()

option(UNIORM_BUILD_TESTS "Build uniorm tests" ON)

find_package(ODBC REQUIRED)
find_package(Threads REQUIRED)

add_library(uniorm SHARED
    src/types.cpp
    src/unicode.cpp
    src/row.cpp
    src/result_set.cpp
    src/params.cpp
    src/connection.cpp
    src/dialect.cpp
    src/transaction.cpp
    src/pool.cpp
    src/query/expression.cpp
    src/query/builder.cpp
    src/mapping/registry.cpp
    src/odbc/error.cpp
    src/odbc/environment.cpp
    src/odbc/connection.cpp
    src/odbc/statement.cpp
)
add_library(uniorm::uniorm ALIAS uniorm)

target_include_directories(uniorm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# ODBC is PUBLIC because the header-only binding layer (registry.hpp ->
# projection.hpp) references ODBC symbols in consumer translation units.
target_link_libraries(uniorm PUBLIC ODBC::ODBC PRIVATE Threads::Threads)
target_compile_definitions(uniorm PRIVATE UNIORM_BUILDING_DLL)
if(UNIORM_DECIMAL_DEFAULT STREQUAL "string")
    target_compile_definitions(uniorm PUBLIC UNIORM_DECIMAL_AS_STRING)
else()
    target_compile_definitions(uniorm PUBLIC UNIORM_DECIMAL_AS_DOUBLE)
endif()

if(UNIORM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
