#pragma once

#include <vector>

#include <sql.h>
#include <sqlext.h>

namespace uniorm::detail {

// Staging buffers that must outlive statement execution after params::bind.
struct param_staging {
  struct slot {
    SQLLEN indicator = 0;
    unsigned char bit = 0;
    SQL_TIMESTAMP_STRUCT ts{};
  };
  std::vector<slot> slots;
};

}  // namespace uniorm::detail
