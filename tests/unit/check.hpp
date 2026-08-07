#pragma once

#include <cstdio>

namespace uniorm::test {

inline int& failure_count() {
  static int count = 0;
  return count;
}

}  // namespace uniorm::test

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++uniorm::test::failure_count();                            \
    }                                                             \
  } while (0)

#define CHECK_THROWS(expr, exception_type)                                 \
  do {                                                                     \
    bool caught_ = false;                                                  \
    try {                                                                  \
      (void)(expr);                                                        \
    } catch (exception_type const&) {                                      \
      caught_ = true;                                                      \
    }                                                                      \
    if (!caught_) {                                                        \
      std::printf("FAIL %s:%d: expected %s from %s\n", __FILE__, __LINE__, \
        #exception_type, #expr);                                           \
      ++uniorm::test::failure_count();                                     \
    }                                                                      \
  } while (0)
