#pragma once

// Private header: OCI diagnostics and error mapping. Not installed; SQL
// failures are classifiable through uniorm::backend::backend_error, which
// this header's type derives from.

#include <string>
#include <vector>

#include <oci.h>

#include "export.hpp"
#include <uniorm/backend/error.hpp>

namespace uniorm::oracle_oci {

// Carries the error records a handle accumulated, under the backend
// name "oracle".
class UNIORM_ORACLE_OCI_API oci_error : public backend::backend_error {
public:
  using diagnostic = backend::backend_error::diagnostic;

  oci_error(std::string const& context, std::vector<diagnostic> diags);
};

// Drain the records attached to an error handle, oldest first. A null
// handle has none. OCIErrorGet does not serve a SQL state, so each
// record's state carries the ORA- / PLS- code that leads its message.
UNIORM_ORACLE_OCI_API std::vector<oci_error::diagnostic> collect_diagnostics(
  OCIError* error);

// The records one handle carries, spelled one after another as
// ": [state] (code) message". A handle with none gives no text, so a caller
// can append the answer to a sentence of its own and read the same either way.
UNIORM_ORACLE_OCI_API std::string diagnostic_text(OCIError* error);

// Check an OCI return code; anything short of success or success-with-info
// collects the handle's records and throws oci_error. `tolerated` names the one
// further code this call reads as normal rather than failed: 0 (OCI_SUCCESS)
// when there is none, since a code already covered by success needs no
// exemption. A fetch that ran out reports OCI_NO_DATA, and a LOB read that
// wants another piece reports OCI_NEED_DATA.
UNIORM_ORACLE_OCI_API void throw_if_error(sword status, OCIError* error,
  std::string const& context, sword tolerated = 0);

}  // namespace uniorm::oracle_oci
