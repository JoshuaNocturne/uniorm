#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef UNIORM_BUILDING_DLL
#define UNIORM_API __declspec(dllexport)
#else
#define UNIORM_API __declspec(dllimport)
#endif
#else
#define UNIORM_API __attribute__((visibility("default")))
#endif
