# Core logic (config/naming/generator/reader) lives in a static lib so the
# unit tests can link it without the CLI main. The reader asks a backend for
# schema metadata through the public interface, so nothing here names a driver.
add_library(uniorm_gen_core STATIC
    config.cpp
    ddl_meta.cpp
    generator.cpp
    naming.cpp
    schema_reader.cpp
)
target_link_libraries(uniorm_gen_core PUBLIC uniorm::uniorm)
target_include_directories(uniorm_gen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Naming folds through the core's one case folding, which is the core's
# private detail; the tool is built in this tree, not from an installed
# prefix, so it reads that file where it lies.
target_include_directories(uniorm_gen_core PUBLIC ${PROJECT_SOURCE_DIR}/src)

add_executable(uniorm-gen main.cpp)
# The generator opens a live connection through registry::create, so it needs
# the ODBC backend loaded as a sibling NEEDED entry on this exe -- the core
# alone carries no scheme. uniorm_gen_core stays driver-free so its unit tests
# can link no backend at all, which is also what lets --input-ddl answer
# without a driver present.
target_link_libraries(uniorm-gen PRIVATE uniorm_gen_core uniorm::odbc)
uniorm_copy_runtime_dlls(uniorm-gen)
