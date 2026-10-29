# The libpq backend is its own shared library for the same reason the ODBC one
# is: the core carries no driver dependency, and a consumer that talks to one
# server should not map a client library it will never load. The core's loader
# searches <libdir>/uniorm/backends/libuniorm_postgresql.so (see
# src/backend/plugin_loader.cpp), so that is where this installs. The
# uniormLibpqTargets export remains for the white-box tests that link the
# backend directly; consumers do not name it.

find_package(PostgreSQL REQUIRED)
# CMake's finder answers with variables, and the imported target it creates is
# directory-scoped by default; the white-box tests reach for libpq-fe.h through
# this backend's private headers, so the client library is named once, globally.
if(NOT TARGET PostgreSQL::libpq)
    add_library(PostgreSQL::libpq UNKNOWN IMPORTED)
    set_target_properties(PostgreSQL::libpq PROPERTIES
        IMPORTED_LOCATION "${PostgreSQL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PostgreSQL_INCLUDE_DIR}"
        IMPORTED_GLOBAL TRUE)
endif()

add_library(uniorm_libpq SHARED
    backend.cpp
    connection.cpp
    error.cpp
    sql_text.cpp
    statement.cpp
)
add_library(uniorm::libpq ALIAS uniorm_libpq)

if(WIN32)
    target_compile_options(uniorm_libpq PRIVATE
        /utf-8)
endif()

target_compile_features(uniorm_libpq PUBLIC cxx_std_20)
target_link_libraries(uniorm_libpq
    PUBLIC uniorm::uniorm
    PRIVATE PostgreSQL::libpq
)
target_compile_definitions(uniorm_libpq PRIVATE
    UNIORM_BUILDING_LIBPQ_DLL)
set_target_properties(uniorm_libpq PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# The loaded file is named for the scheme the backend registers under, not for
# this directory: the core dlopens libuniorm_<scheme>.so on a scheme miss.
set_target_properties(uniorm_libpq PROPERTIES
    OUTPUT_NAME uniorm_postgresql
    EXPORT_NAME libpq
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    if(UNIX AND NOT APPLE AND NOT CYGWIN)
        # Installed under <libdir>/uniorm/backends/; the core library is one
        # directory up from that, so ../.. gets it back to the plain libdir.
        set_target_properties(uniorm_libpq PROPERTIES
            INSTALL_RPATH "$ORIGIN/../.."
        )
    endif()
    install(TARGETS uniorm_libpq
        EXPORT uniormLibpqTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/uniorm/backends
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/uniorm/backends
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/uniorm/backends
    )
    install(EXPORT uniormLibpqTargets
        FILE uniormLibpqTargets.cmake
        NAMESPACE uniorm::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uniorm
    )
endif()
