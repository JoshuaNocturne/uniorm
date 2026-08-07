#pragma once

#include "detail/handles.hpp"
#include "error.hpp"

namespace uniorm::odbc {

// RAII wrapper for SQLHENV. Typically one per process.
class UNIORM_API environment {
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

// Process-wide shared environment, created on first use.
UNIORM_API environment& shared_environment();

}  // namespace uniorm::odbc
