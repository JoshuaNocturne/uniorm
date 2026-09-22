cmake_minimum_required(VERSION 3.20)
project(uniorm_install_consumer CXX)

# This project deliberately sets no CMAKE_CXX_STANDARD: the C++20 requirement
# has to arrive through the imported target, or a consumer that forgets to
# check would fail on the first public header. Guarded so the check cannot be
# weakened by someone making this project build again.
if(CMAKE_CXX_STANDARD)
    message(FATAL_ERROR
        "the consumer must not set a standard; the imported target carries it")
endif()

# The link line is the same in both smoke shapes -- uniorm::uniorm alone. The
# two cases differ only in whether the plugin directory contains
# libuniorm_odbc.so, and the caller tells us which shape we're in via
# SMOKE_UNIORM_HAS_ODBC so main.cpp can assert accordingly.
find_package(uniorm ${SMOKE_UNIORM_VERSION} CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE uniorm::uniorm)
if(SMOKE_UNIORM_HAS_ODBC)
    target_compile_definitions(consumer PRIVATE UNIORM_SMOKE_HAS_ODBC)
endif()

enable_testing()
add_test(NAME consumer COMMAND consumer)
if(SMOKE_UNIORM_PLUGIN_DIR)
    # The core was configured with a bake-time absolute UNIORM_PLUGIN_DIR
    # pointing at whichever prefix built it, which is not this throwaway
    # prefix. The env override redirects the loader at the plugin file this
    # smoke run just installed.
    set_tests_properties(consumer PROPERTIES
        ENVIRONMENT "UNIORM_PLUGIN_PATH=${SMOKE_UNIORM_PLUGIN_DIR}")
endif()
