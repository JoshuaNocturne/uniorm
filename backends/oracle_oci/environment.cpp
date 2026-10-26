#include "environment.hpp"

#include "error.hpp"

namespace uniorm::oracle_oci {

namespace {

// Only UTF-16 has a documented id in the SDK headers; the others are read
// from the client, so neither name is spelled with a number here.
ub2 lookup_id(OCIEnv* env, char const* name) {
  return OCINlsCharSetNameToId(
    env, reinterpret_cast<OraText const*>(name));
}

// The ids are needed to create the environment that is asked for them, so a
// throwaway environment built on the site's NLS_LANG answers first. Guessing
// AL32UTF8's id would bind every text column through the wrong charset and
// corrupt it silently, which is worse than one extra handle at startup.
void charset_ids(ub2& declared, ub2& national) {
  OCIEnv* bootstrap = nullptr;
  if (OCIEnvCreate(&bootstrap, OCI_THREADED, nullptr, nullptr, nullptr,
        nullptr, 0, nullptr) != OCI_SUCCESS) {
    throw oci_error("create an OCI environment to look up charset ids", {});
  }
  ub2 const utf8 = lookup_id(bootstrap, "AL32UTF8");
  ub2 const utf16 = lookup_id(bootstrap, "AL16UTF16");
  OCIHandleFree(bootstrap, OCI_HTYPE_ENV);
  if (utf8 == 0 || utf16 == 0) {
    throw oci_error("look up the AL32UTF8 and AL16UTF16 charset ids", {});
  }
  declared = utf8;
  national = utf16;
}

}  // namespace

environment::environment() {
  ub2 declared = 0;
  ub2 national = 0;
  charset_ids(declared, national);
  OCIEnv* created = nullptr;
  sword const status = OCIEnvNlsCreate(&created, OCI_THREADED | OCI_OBJECT,
    nullptr, nullptr, nullptr, nullptr, 0, nullptr, declared, national);
  if (status != OCI_SUCCESS || created == nullptr) {
    throw oci_error("create the OCI environment", {});
  }
  handle_ = created;
}

environment::~environment() {
  if (handle_ != nullptr) {
    OCIHandleFree(handle_, OCI_HTYPE_ENV);
  }
}

std::shared_ptr<environment> shared_environment() {
  // A session can log off from a static destructor in the library that loaded
  // this one, which runs after the statics here. The environment has to
  // outlive every session naming it, or OCILogoff dies on a freed parent.
  static std::shared_ptr<environment> env = std::make_shared<environment>();
  return env;
}

}  // namespace uniorm::oracle_oci
