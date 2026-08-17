#include <uniorm/backend/error.hpp>

namespace uniorm::backend {

backend_error::backend_error(std::string backend, std::string context,
  std::vector<diagnostic> diags)
  : uniorm_error([&] {
      std::string msg = context;
      for (auto const& d : diags) {
        msg += "\n  [" + d.state + "] (" + std::to_string(d.native_code) +
               ") " + d.message;
      }
      return msg;
    }()),
    backend_(std::move(backend)),
    diags_(std::move(diags)) {}

}  // namespace uniorm::backend
