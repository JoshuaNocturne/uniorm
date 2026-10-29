#pragma once

// Private header: libpq diagnostics and error mapping. Not installed; SQL
// failures are classifiable through uniorm::backend::backend_error, which this
// header's type derives from.

#include <initializer_list>
#include <string>
#include <vector>

#include <libpq-fe.h>

#include <uniorm/backend/error.hpp>
#include "export.hpp"

namespace uniorm::libpq {

// Carries the fields one libpq failure reported, under the backend name
// "postgresql" -- the scheme this plugin registers, which is what callers
// compare a failure against.
class UNIORM_LIBPQ_API pq_error : public backend::backend_error {
public:
  using diagnostic = backend::backend_error::diagnostic;

  pq_error(std::string const& context, std::vector<diagnostic> diags);
};

// What the server reported about a failed result: its SQLSTATE and primary
// message, then the detail and hint it sent along. An empty SQLSTATE is a
// failure the server never named, which a client-side error is.
UNIORM_LIBPQ_API std::vector<pq_error::diagnostic> collect_diagnostics(
  PGresult const* result);

// A connection that could not be made reports through itself, in prose with no
// SQLSTATE to carry it.
UNIORM_LIBPQ_API std::vector<pq_error::diagnostic> collect_diagnostics(
  PGconn const* connection);

// Throw unless the result reports one of `ok`. A statement that returns
// nothing answers PGRES_COMMAND_OK and a query PGRES_TUPLES_OK, so a call that
// may do either names both.
UNIORM_LIBPQ_API void throw_if_failed(PGresult const* result,
  std::string const& context, std::initializer_list<ExecStatusType> ok);

}  // namespace uniorm::libpq
