#pragma once

// Private header: ODBC environment handle. Not installed.

#include <memory>

#include "error.hpp"
#include "export.hpp"
#include "handles.hpp"

namespace uniorm::odbc {

// RAII wrapper for SQLHENV. Typically one per process.
class UNIORM_ODBC_API environment {
public:
  environment();
  ~environment();

  environment(environment&&) noexcept;
  environment& operator=(environment&&) noexcept;

  environment(environment const&) = delete;
  environment& operator=(environment const&) = delete;

  SQLHENV native() const noexcept {
    return static_cast<SQLHENV>(handle_.get());
  }

private:
  detail::env_handle handle_;
};

// Process-wide shared environment, created on first use. Each connection keeps
// a copy of the pointer, so the environment outlives every connection even when
// a static one is destroyed after the shared handle itself.
UNIORM_ODBC_API std::shared_ptr<environment> shared_environment();

}  // namespace uniorm::odbc
