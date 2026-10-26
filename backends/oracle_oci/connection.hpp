#pragma once

// Private header: the OCI session. Not installed.

#include <string>
#include <string_view>

#include <oci.h>

#include "environment.hpp"
#include "error.hpp"
#include "handles.hpp"

namespace uniorm::oracle_oci {

// One logged-on session: the service context OCILogon2 hands back, plus the
// error handle every call on this connection reports into. Move-only. The
// logoff releases the server, session and context handles together, so none
// of the three is allocated or freed here.
class UNIORM_ORACLE_OCI_API connection {
public:
  connection();
  ~connection();

  connection(connection&&) noexcept;
  connection& operator=(connection&&) noexcept;

  connection(connection const&) = delete;
  connection& operator=(connection const&) = delete;

  // "[user[/password]]@connect", the tail of an "oracle://" string. The
  // credential half is cut at the last '@', since a connect descriptor holds
  // none while a password may. With no '@' at all there are no credentials
  // and the server's external authentication answers for them.
  void open(std::string_view tail);
  void open(std::string_view connect, std::string_view user,
    std::string_view password);
  void close();
  bool is_open() const noexcept {
    return service_ != nullptr;
  }

  // OCI has no autocommit attribute to set: the flag decides the mode each
  // execute passes, and turning it off is what makes a transaction. Turning it
  // on ends the open one, which is what the ODBC attribute does.
  void set_autocommit(bool enabled);
  bool autocommit() const noexcept {
    return autocommit_;
  }
  void commit();
  void rollback();

  OCISvcCtx* service() const noexcept {
    return service_;
  }
  OCIError* error() const noexcept {
    return error_.get();
  }
  OCIEnv* env() const noexcept {
    return env_->native();
  }

private:
  void logon(std::string_view connect, std::string_view user,
    std::string_view password);

  std::shared_ptr<environment> env_;
  detail::error_handle error_;
  OCISvcCtx* service_ = nullptr;
  bool autocommit_ = true;
};

}  // namespace uniorm::oracle_oci
