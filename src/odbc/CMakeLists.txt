# The ODBC backend is its own shared library so a consumer that talks to one
# server does not map drivers it will never touch. The core links no ODBC;
# libodbc's NEEDED entry lives here instead. Loading is by dlopen on scheme
# miss (see src/backend/plugin_loader.cpp): the core searches
# <libdir>/uniorm/backends/libuniorm_odbc.so, so this library installs there
# rather than next to the core. The uniormOdbcTargets export remains for the
# white-box tests that link the backend directly; consumers do not name it.

find_package(ODBC REQUIRED)
# The imported target FindODBC creates is directory-scoped by default; promote
# it to global so a white-box test that reaches for the SQL* headers it points
# at -- outside this directory -- can still name it.
if(TARGET ODBC::ODBC AND NOT CMAKE_VERSION VERSION_LESS 3.11)
    set_property(TARGET ODBC::ODBC PROPERTY IMPORTED_GLOBAL TRUE)
endif()

add_library(uniorm_odbc SHARED
    backend.cpp
    connection.cpp
    environment.cpp
    error.cpp
    schema_catalog.cpp
    statement.cpp
)
add_library(uniorm::odbc ALIAS uniorm_odbc)

target_compile_features(uniorm_odbc PUBLIC cxx_std_20)
target_link_libraries(uniorm_odbc
    PUBLIC uniorm::uniorm
    PRIVATE ODBC::ODBC
)
target_compile_definitions(uniorm_odbc PRIVATE UNIORM_BUILDING_ODBC_DLL)
set_target_properties(uniorm_odbc PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

set_target_properties(uniorm_odbc PROPERTIES
    EXPORT_NAME odbc
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    if(UNIX AND NOT APPLE AND NOT CYGWIN)
        # Installed under <libdir>/uniorm/backends/; the core library is one
        # directory up from that, so ../.. gets it back to the plain libdir.
        set_target_properties(uniorm_odbc PROPERTIES
            INSTALL_RPATH "$ORIGIN/../.."
        )
    endif()
    install(TARGETS uniorm_odbc
        EXPORT uniormOdbcTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/uniorm/backends
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/uniorm/backends
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/uniorm/backends
    )
    install(EXPORT uniormOdbcTargets
        FILE uniormOdbcTargets.cmake
        NAMESPACE uniorm::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uniorm
    )
endif()
