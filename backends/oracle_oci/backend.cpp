#include "backend.hpp"

#include <cstring>
#include <memory>
#include <string>

#include <oci.h>

#include <uniorm/backend/plugin.hpp>
#include <uniorm/backend/registry.hpp>

#include "error.hpp"
#include "statement.hpp"

namespace uniorm::oracle_oci {

void oci_connection::open(std::string_view connection_string) {
  conn_.open(connection_string);
}

// The four pieces go to OCI as four arguments, so nothing here has to spell
// them as text and re-read the spelling: the target is the connect string, an
// empty user is the server's own authentication.
void oci_connection::open(connection_spec const& spec) {
  conn_.open(spec.target, spec.user, spec.password);
}

void oci_connection::close() {
  conn_.close();
}

bool oci_connection::is_open() const noexcept {
  return conn_.is_open();
}

void oci_connection::set_autocommit(bool enabled) {
  conn_.set_autocommit(enabled);
}

void oci_connection::commit() {
  conn_.commit();
}

void oci_connection::rollback() {
  conn_.rollback();
}

// Neither fast path is claimed: a batch here is one execute per row with the
// counts added up, so the core's row-by-row route is the only one it owes.
backend::capabilities oci_connection::caps() const noexcept {
  return {/*columnar_batch=*/false, /*array_rowcount_totals=*/false};
}

std::string oci_connection::dbms_name() const {
  OraText banner[512] = {};
  throw_if_error(OCIServerVersion(conn_.service(), conn_.error(), banner,
                     static_cast<ub4>(sizeof(banner)), OCI_HTYPE_SVCCTX),
    conn_.error(), "read the server version");
  char const* const text = reinterpret_cast<char const*>(banner);
  return std::string(text, std::strlen(text));
}

std::unique_ptr<backend::statement_iface>
oci_connection::create_statement() {
  return std::make_unique<oci_statement>(conn_);
}

// The service context, which is what an OCI caller outside this library needs
// to keep using the same session.
void* oci_connection::native_handle() noexcept {
  return conn_.service();
}

}  // namespace uniorm::oracle_oci

// The plugin contract -- see include/uniorm/backend/plugin.hpp. The core's
// dlopen loader calls these on scheme miss; nothing registers at load time.
extern "C" UNIORM_ORACLE_OCI_API uint32_t uniorm_plugin_abi_version() {
  return UNIORM_ABI_VERSION;
}

extern "C" UNIORM_ORACLE_OCI_API void uniorm_plugin_register(
  uniorm::backend::registry& reg) {
  reg.register_backend("oracle", [] {
    return std::make_unique<uniorm::oracle_oci::oci_connection>();
  });
}
