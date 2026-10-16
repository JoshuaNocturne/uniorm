#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <uniorm/error.hpp>
#include <uniorm/export.hpp>

namespace uniorm::backend {

// Failure reported by a backend implementation. Carries the backend
// name plus backend-native diagnostics; what() renders context followed
// by one "[state] (native_code) message" line per diagnostic.
class UNIORM_API backend_error : public uniorm_error {
public:
  struct diagnostic {
    std::string state;
    std::int64_t native_code = 0;
    std::string message;
  };

  backend_error(std::string backend, std::string context,
    std::vector<diagnostic> diags);

  std::string const& backend_name() const noexcept {
    return backend_;
  }
  std::vector<diagnostic> const& diagnostics() const noexcept {
    return diags_;
  }

private:
  std::string backend_;
  std::vector<diagnostic> diags_;
};

// A read asked of a backend that cannot serve it and that has no slower
// route to fall back on: table introspection, and an open by connection
// pieces. The capability flags are the opposite case; those let the core
// take a slower path instead.
class UNIORM_API capability_not_supported : public uniorm_error {
public:
  using uniorm_error::uniorm_error;
};

// No backend serves the scheme: the connection string's prefix names none
// that is registered, or a connection spec left the field empty.
class UNIORM_API unknown_scheme : public uniorm_error {
public:
  using uniorm_error::uniorm_error;
};

}  // namespace uniorm::backend
