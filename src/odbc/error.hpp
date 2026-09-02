#pragma once

// Private header: ODBC diagnostics and error mapping. Not installed;
// SQL failures are classifiable through uniorm::backend::backend_error,
// which this header's type derives from.

#include <string>
#include <vector>

#include <sql.h>
#include <sqlext.h>

#include <uniorm/backend/error.hpp>
#include <uniorm/export.hpp>

namespace uniorm::odbc {

// Carries the driver's diagnostic records under the backend name "odbc".
class UNIORM_API odbc_error : public backend::backend_error {
public:
  using diagnostic = backend::backend_error::diagnostic;

  odbc_error(std::string const& context, std::vector<diagnostic> diags);
};

// Collect all diagnostic records attached to a handle.
UNIORM_API std::vector<odbc_error::diagnostic> collect_diagnostics(
  SQLSMALLINT handle_type, SQLHANDLE handle);

// Check an ODBC return code; on failure (anything but SQL_SUCCESS /
// SQL_SUCCESS_WITH_INFO / the explicitly tolerated codes) collect diagnostics
// and throw odbc_error. SQL_NO_DATA is tolerated where expected by passing
// tolerate_no_data = true.
UNIORM_API void throw_if_error(SQLRETURN rc, SQLSMALLINT handle_type,
  SQLHANDLE handle, std::string const& context, bool tolerate_no_data = false);

}  // namespace uniorm::odbc
