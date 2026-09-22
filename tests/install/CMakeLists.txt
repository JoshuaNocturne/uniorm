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

# SMOKE_UNIORM_COMPONENTS is empty for the core-only shape and "odbc" for the
# backend shape. The find_package call is the same either way: a missing
# component would fail through check_required_components with a message naming
# why, not silently.
find_package(uniorm ${SMOKE_UNIORM_VERSION} CONFIG REQUIRED
    ${SMOKE_UNIORM_COMPONENTS})

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE uniorm::uniorm)
if("odbc" IN_LIST SMOKE_UNIORM_COMPONENTS)
    target_link_libraries(consumer PRIVATE uniorm::odbc)
    target_compile_definitions(consumer PRIVATE UNIORM_SMOKE_HAS_ODBC)
endif()

enable_testing()
add_test(NAME consumer COMMAND consumer)
