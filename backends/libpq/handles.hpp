#pragma once

// Private header: the two libpq handles this backend owns, each freed by the
// type that holds it. Not installed.

#include <utility>

#include <libpq-fe.h>

#include "export.hpp"

namespace uniorm::libpq {
namespace detail {

// One result, released with PQclear when this goes out of scope. libpq hands a
// result to every call that reaches the server, including the ones whose only
// answer is a status word.
class UNIORM_LIBPQ_API result_holder {
public:
  explicit result_holder(PGresult* owned = nullptr) noexcept
    : owned_(owned) {}
  ~result_holder() {
    clear();
  }

  result_holder(result_holder&& other) noexcept
    : owned_(std::exchange(other.owned_, nullptr)) {}

  result_holder& operator=(result_holder&& other) noexcept {
    if (this != &other) {
      clear();
      owned_ = std::exchange(other.owned_, nullptr);
    }
    return *this;
  }

  result_holder(result_holder const&) = delete;
  result_holder& operator=(result_holder const&) = delete;

  PGresult* get() const noexcept {
    return owned_;
  }
  void clear() noexcept {
    if (owned_ != nullptr) {
      PQclear(owned_);
      owned_ = nullptr;
    }
  }

private:
  PGresult* owned_ = nullptr;
};

}  // namespace detail
}  // namespace uniorm::libpq
