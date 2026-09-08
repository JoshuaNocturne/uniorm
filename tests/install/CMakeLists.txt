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

find_package(uniorm ${SMOKE_UNIORM_VERSION} CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE uniorm::uniorm)
if(SMOKE_HAS_ODBC)
    target_compile_definitions(consumer PRIVATE UNIORM_SMOKE_HAS_ODBC)
endif()

enable_testing()
add_test(NAME consumer COMMAND consumer)
