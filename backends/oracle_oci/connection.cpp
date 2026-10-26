#include "connection.hpp"

#include <utility>

#include <uniorm/backend/error.hpp>

namespace uniorm::oracle_oci {

namespace {

// An empty piece has no bytes to point at, and OCI reads a null pointer as
// "none" where an empty one would be a blank value.
OraText const* text_of(std::string_view value) {
  return value.empty()
    ? nullptr
    : reinterpret_cast<OraText const*>(value.data());
}

ub4 length_of(std::string_view value) {
  return static_cast<ub4>(value.size());
}

}  // namespace

connection::connection()
  : env_(shared_environment()),
    error_(env_->native(), "session diagnostics") {}

connection::~connection() {
  if (service_ != nullptr) {
    // Best effort: a session that will not log off cleanly still dies with
    // this object, and a destructor that throws would end the process.
    OCILogoff(service_, error_.get());
    service_ = nullptr;
  }
}

// The environment is copied, not moved: a moved-from connection still answers
// env(), and no connection may be the one that frees the shared parent.
connection::connection(connection&& other) noexcept
  : env_(other.env_), error_(std::move(other.error_)),
    service_(std::exchange(other.service_, nullptr)),
    autocommit_(other.autocommit_) {}

connection& connection::operator=(connection&& other) noexcept {
  if (this != &other) {
    if (service_ != nullptr) {
      OCILogoff(service_, error_.get());
    }
    env_ = other.env_;
    error_ = std::move(other.error_);
    service_ = std::exchange(other.service_, nullptr);
    autocommit_ = other.autocommit_;
  }
  return *this;
}

void connection::logon(std::string_view connect, std::string_view user,
  std::string_view password) {
  if (connect.empty()) {
    throw backend::backend_error(
      "oracle", "an Oracle session needs a connect string", {});
  }
  // Credentials given means the server authenticates them; none given means
  // the server's external authentication answers for the session.
  ub4 const mode = user.empty() ? OCI_CRED_EXT : OCI_DEFAULT;
  sword const status = OCILogon2(env_->native(), error_.get(), &service_,
    text_of(user), length_of(user), text_of(password), length_of(password),
    text_of(connect), length_of(connect), mode);
  if (status != OCI_SUCCESS) {
    if (service_ != nullptr) {
      // A refused logon still hands back the context it allocated. It owns no
      // session, so OCILogoff has nothing to release and OCIHandleFree is what
      // takes it; dropping the pointer is what keeps is_open() telling the
      // truth about a session that was never made.
      OCIHandleFree(service_, OCI_HTYPE_SVCCTX);
      service_ = nullptr;
    }
    throw oci_error("log on to " + std::string(connect),
      collect_diagnostics(error_.get()));
  }
}

void connection::open(std::string_view tail) {
  std::size_t const at = tail.rfind('@');
  if (at == std::string_view::npos) {
    logon(tail, {}, {});
    return;
  }
  std::string_view const credentials = tail.substr(0, at);
  std::size_t const slash = credentials.find('/');
  if (slash == std::string_view::npos) {
    logon(tail.substr(at + 1), credentials, {});
    return;
  }
  logon(tail.substr(at + 1), credentials.substr(0, slash),
    credentials.substr(slash + 1));
}

void connection::open(std::string_view connect, std::string_view user,
  std::string_view password) {
  logon(connect, user, password);
}

void connection::close() {
  if (service_ == nullptr) {
    return;
  }
  OCISvcCtx* const session = std::exchange(service_, nullptr);
  sword const status = OCILogoff(session, error_.get());
  if (status != OCI_SUCCESS) {
    // The handles belong to the logoff that failed, so they cannot be
    // released here without a second free; the session is dropped instead.
    throw oci_error("log off", collect_diagnostics(error_.get()));
  }
}

void connection::set_autocommit(bool enabled) {
  if (enabled && !autocommit_ && service_ != nullptr) {
    // A manual-mode session has no commit coming on its own: the switch is
    // what ends the transaction its pending work sits in.
    commit();
  }
  autocommit_ = enabled;
}

void connection::commit() {
  throw_if_error(OCITransCommit(
                     service_, error_.get(), OCI_DEFAULT), error_.get(),
    "commit transaction");
}

void connection::rollback() {
  throw_if_error(OCITransRollback(
                     service_, error_.get(), OCI_DEFAULT), error_.get(),
    "roll back transaction");
}

}  // namespace uniorm::oracle_oci
