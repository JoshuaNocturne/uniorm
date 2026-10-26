# The Oracle OCI backend is its own shared library for the same reason the ODBC
# one is: the core carries no driver dependency, and a consumer that talks to
# one server should not map a client library it will never load. The core's
# loader searches <libdir>/uniorm/backends/libuniorm_oracle.so (see
# src/backend/plugin_loader.cpp), so that is where this installs. The
# uniormOracleOciTargets export remains for the white-box tests that link the
# backend directly; consumers do not name it.

find_package(OracleOCI REQUIRED)
# The imported target this module creates is directory-scoped by default;
# promote it to global so a white-box test that reaches for the OCI headers
# the backend's private ones include -- outside this directory -- can name it.
if(TARGET Oracle::OCI AND NOT CMAKE_VERSION VERSION_LESS 3.11)
    set_property(TARGET Oracle::OCI PROPERTY IMPORTED_GLOBAL TRUE)
endif()

add_library(uniorm_oracle_oci SHARED
    backend.cpp
    connection.cpp
    environment.cpp
    error.cpp
    sql_text.cpp
    statement.cpp
)
add_library(uniorm::oracle_oci ALIAS uniorm_oracle_oci)

if(WIN32)
    target_compile_options(uniorm_oracle_oci PRIVATE
        /utf-8)
endif()

target_compile_features(uniorm_oracle_oci PUBLIC cxx_std_20)
target_link_libraries(uniorm_oracle_oci
    PUBLIC uniorm::uniorm
    PRIVATE Oracle::OCI
)
target_compile_definitions(uniorm_oracle_oci PRIVATE
    UNIORM_BUILDING_ORACLE_OCI_DLL)
set_target_properties(uniorm_oracle_oci PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# The loaded file is named for the scheme the backend registers under, not for
# this directory: the core dlopens libuniorm_<scheme>.so on a scheme miss.
set_target_properties(uniorm_oracle_oci PROPERTIES
    OUTPUT_NAME uniorm_oracle
    EXPORT_NAME oracle_oci
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    if(UNIX AND NOT APPLE AND NOT CYGWIN)
        # Installed under <libdir>/uniorm/backends/; the core library is one
        # directory up from that, so ../.. gets it back to the plain libdir.
        set_target_properties(uniorm_oracle_oci PROPERTIES
            INSTALL_RPATH "$ORIGIN/../.."
        )
    endif()
    install(TARGETS uniorm_oracle_oci
        EXPORT uniormOracleOciTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/uniorm/backends
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/uniorm/backends
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/uniorm/backends
    )
    install(EXPORT uniormOracleOciTargets
        FILE uniormOracleOciTargets.cmake
        NAMESPACE uniorm::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uniorm
    )
endif()
