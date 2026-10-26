#pragma once

// Private header: the OCI environment handle. Not installed.

#include <memory>

#include <oci.h>

#include "export.hpp"

namespace uniorm::oracle_oci {

// Process-wide OCI environment, created on first use. Every handle and
// descriptor this backend allocates names it as its parent. It is created
// with an explicit AL32UTF8 database charset and AL16UTF16 national charset
// rather than the site's NLS_LANG, because the interface contract is UTF-8 in
// and UTF-8 out on every channel -- and a column fetched as text is converted
// to that charset by OCI, so no read here has to ask which one it got.
class UNIORM_ORACLE_OCI_API environment {
public:
  environment();
  ~environment();

  environment(environment const&) = delete;
  environment& operator=(environment const&) = delete;

  OCIEnv* native() const noexcept {
    return handle_;
  }

private:
  OCIEnv* handle_ = nullptr;
};

UNIORM_ORACLE_OCI_API std::shared_ptr<environment> shared_environment();

}  // namespace uniorm::oracle_oci
