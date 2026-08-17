#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/export.hpp>

namespace uniorm::backend {

struct parsed_scheme {
  std::string scheme;      // lowercase; "odbc" for bare strings
  std::string_view tail;   // everything after "scheme://"
};

// Splits a connection string into backend scheme and backend-specific
// tail. Strings without a valid "scheme://" prefix (including ODBC
// quirks like "SERVER=tcp://host") are treated as bare ODBC strings.
UNIORM_API parsed_scheme parse_scheme(std::string_view connection_string);

// Runtime registry mapping schemes to backend factories. Backends
// compiled into libuniorm self-register at library load; out-of-tree
// backends call register_backend at startup.
class UNIORM_API registry {
public:
  using factory = std::function<std::unique_ptr<connection_iface>()>;

  static registry& instance();

  // Throws backend_error when the scheme is already registered.
  void register_backend(std::string scheme, factory f);
  bool contains(std::string_view scheme) const;
  std::vector<std::string> schemes() const;

  // Parses the scheme, constructs an unopened backend connection, and
  // writes the scheme-specific tail to *tail (when non-null). Throws
  // unknown_scheme listing the registered schemes when unregistered.
  std::unique_ptr<connection_iface> create(
    std::string_view connection_string, std::string* tail) const;

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, factory> backends_;
};

// File-scope registration helper:
//   static backend::registrar reg("odbc", [] { return ...; });
struct UNIORM_API registrar {
  registrar(std::string scheme, registry::factory f);
};

}  // namespace uniorm::backend
