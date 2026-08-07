#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <sql.h>
#include <sqlext.h>

#include "../error.hpp"
#include "../export.hpp"

namespace uniorm::odbc {

class UNIORM_API odbc_error : public uniorm_error {
public:
  struct diagnostic {
    std::string state;  // 5-char SQLSTATE
    std::int64_t native_code = 0;
    std::string message;
  };

  odbc_error(std::string const& context, std::vector<diagnostic> diags);

  std::vector<diagnostic> const& diagnostics() const noexcept {
    return diags_;
  }

private:
  std::vector<diagnostic> diags_;
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
