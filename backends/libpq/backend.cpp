#include "backend.hpp"

#include <memory>
#include <string>

#include <libpq-fe.h>

#include <uniorm/backend/plugin.hpp>
#include <uniorm/backend/registry.hpp>

#include "error.hpp"
#include "statement.hpp"

namespace uniorm::libpq {

void pq_connection::open(std::string_view connection_string) {
  conn_.open(connection_string);
}

// The pieces go to libpq as the keywords it names them under, so a password
// holding a space or a quote is one value rather than a parse of its own.
void pq_connection::open(connection_spec const& spec) {
  conn_.open(spec.target, spec.user, spec.password);
}

void pq_connection::close() {
  conn_.close();
}

bool pq_connection::is_open() const noexcept {
  return conn_.is_open();
}

void pq_connection::set_autocommit(bool enabled) {
  conn_.set_autocommit(enabled);
}

void pq_connection::commit() {
  conn_.commit();
}

void pq_connection::rollback() {
  conn_.rollback();
}

// Neither fast path is claimed: a batch here is one execute per row with the
// counts added up, so the core's row-by-row route is the only one it owes.
backend::capabilities pq_connection::caps() const noexcept {
  return {/*columnar_batch=*/false, /*array_rowcount_totals=*/false};
}

// The version the server reported at connect time, under the one name this
// library's callers compare a dialect against.
std::string pq_connection::dbms_name() const {
  PGconn const* const session = conn_.native();
  if (session == nullptr) {
    throw pq_error("read the server version of a closed session", {});
  }
  char const* const version = PQparameterStatus(session, "server_version");
  std::string name = "PostgreSQL";
  if (version != nullptr) {
    name += " ";
    name += version;
  }
  return name;
}

std::unique_ptr<backend::statement_iface>
pq_connection::create_statement() {
  return std::make_unique<pq_statement>(conn_);
}

// The connected session, which is what a caller outside this library needs to
// keep using the same transaction.
void* pq_connection::native_handle() noexcept {
  return conn_.native();
}

}  // namespace uniorm::libpq

// The plugin contract -- see include/uniorm/backend/plugin.hpp. The core's
// dlopen loader calls these on scheme miss; nothing registers at load time.
extern "C" UNIORM_LIBPQ_API uint32_t uniorm_plugin_abi_version() {
  return UNIORM_ABI_VERSION;
}

extern "C" UNIORM_LIBPQ_API void uniorm_plugin_register(
  uniorm::backend::registry& reg) {
  reg.register_backend("postgresql", [] {
    return std::make_unique<uniorm::libpq::pq_connection>();
  });
}
