#pragma once

// Private header: symbol-visibility macro for the libpq backend's own shared
// library. Not installed. The core uses UNIORM_API from <uniorm/export.hpp>;
// the backend needs its own so a Windows build imports core symbols while
// still exporting its own.

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef UNIORM_BUILDING_LIBPQ_DLL
#define UNIORM_LIBPQ_API __declspec(dllexport)
#else
#define UNIORM_LIBPQ_API __declspec(dllimport)
#endif
#else
#define UNIORM_LIBPQ_API __attribute__((visibility("default")))
#endif
