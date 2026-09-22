# Core logic (config/naming/generator/reader) lives in a static lib so the
# unit tests can link it without the CLI main. The reader asks a backend for
# schema metadata through the public interface, so nothing here names a driver.
add_library(uniorm_gen_core STATIC
    config.cpp
    generator.cpp
    naming.cpp
    schema_reader.cpp
)
target_link_libraries(uniorm_gen_core PUBLIC uniorm::uniorm)
target_include_directories(uniorm_gen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(uniorm-gen main.cpp)
# The generator opens a connection through registry::create, so it needs the
# ODBC backend loaded as a sibling NEEDED entry on this exe -- the core alone
# carries no scheme. uniorm_gen_core stays driver-free so its unit tests can
# link no backend at all.
target_link_libraries(uniorm-gen PRIVATE uniorm_gen_core uniorm::odbc)
