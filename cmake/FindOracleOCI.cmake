# Locate Oracle Call Interface: the SDK headers and the client library that
# belongs with them. Defines Oracle::OCI for the backend that links it.
#
# The search starts where the vendor puts the SDK: a full client keeps headers
# in <root>/include and its library in <root>/lib, an Instant Client keeps them
# in <root>/sdk/include with the library loose at <root>. OCI_ROOT names a tree
# to look in before the environment's ORACLE_HOME.

set(OracleOCI_HINTS ${OCI_ROOT} $ENV{OCI_ROOT} $ENV{ORACLE_HOME})

find_path(OracleOCI_INCLUDE_DIR NAMES oci.h
    HINTS ${OracleOCI_HINTS}
    PATHS /usr/include
    PATH_SUFFIXES sdk/include include
)

# The RPM layout hides the headers one level per version, so a glob finds what
# no fixed suffix can.
if(NOT OracleOCI_INCLUDE_DIR)
    file(GLOB OracleOCI_INCLUDE_CANDIDATES
        /usr/include/oracle/*/client64 /usr/include/oracle/*/*/client64)
    if(OracleOCI_INCLUDE_CANDIDATES)
        list(SORT OracleOCI_INCLUDE_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
        list(GET OracleOCI_INCLUDE_CANDIDATES 0 OracleOCI_INCLUDE_DIR)
    endif()
    set(OracleOCI_INCLUDE_DIR "${OracleOCI_INCLUDE_DIR}" CACHE PATH
        "Directory holding oci.h")
endif()

if(MSVC)
    set(OracleOCI_NAMES oci)
else()
    set(OracleOCI_NAMES clntsh)
endif()

find_library(OracleOCI_LIBRARY NAMES ${OracleOCI_NAMES}
    HINTS ${OracleOCI_HINTS} $ENV{LD_LIBRARY_PATH} $ENV{LIBRARY_PATH}
    PATHS /usr/lib /usr/lib/oracle
    PATH_SUFFIXES lib sdk/lib
)

if(NOT OracleOCI_LIBRARY)
    file(GLOB OracleOCI_LIBRARY_CANDIDATES
        /usr/lib/oracle/*/client64/lib/libclntsh.so
        /usr/lib/oracle/*/*/client64/lib/libclntsh.so)
    if(OracleOCI_LIBRARY_CANDIDATES)
        list(SORT OracleOCI_LIBRARY_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
        list(GET OracleOCI_LIBRARY_CANDIDATES 0 OracleOCI_LIBRARY)
    endif()
    set(OracleOCI_LIBRARY "${OracleOCI_LIBRARY}" CACHE FILEPATH
        "Oracle client library")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OracleOCI
    REQUIRED_VARS OracleOCI_LIBRARY OracleOCI_INCLUDE_DIR)

if(OracleOCI_FOUND AND NOT TARGET Oracle::OCI)
    add_library(Oracle::OCI UNKNOWN IMPORTED)
    set_target_properties(Oracle::OCI PROPERTIES
        IMPORTED_LOCATION "${OracleOCI_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OracleOCI_INCLUDE_DIR}")
endif()

mark_as_advanced(OracleOCI_INCLUDE_DIR OracleOCI_LIBRARY)
