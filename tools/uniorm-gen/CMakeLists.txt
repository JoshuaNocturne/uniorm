# Core logic (config/naming/generator/reader) lives in a static lib so the
# unit tests can link it without the CLI main.
add_library(uniorm_gen_core STATIC
    config.cpp
    generator.cpp
    naming.cpp
    schema_reader.cpp
)
target_link_libraries(uniorm_gen_core PUBLIC uniorm::uniorm)
target_include_directories(uniorm_gen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(uniorm-gen main.cpp)
target_link_libraries(uniorm-gen PRIVATE uniorm_gen_core)
