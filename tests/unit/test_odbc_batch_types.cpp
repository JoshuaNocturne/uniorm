#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "odbc/backend.hpp"

#include <uniorm/backend/error.hpp>

#include "check.hpp"

using namespace uniorm;

void test_odbc_batch_types() {
  // A column whose non-NULL values all share one shape binds whole: NULL
  // rows are the batch spelling for a missing value, not a shape change.
  {
    std::vector<params> rows{ params(std::int32_t(1)), params(std::monostate{}),
      params(std::int32_t(3)) };
    odbc::check_batch_column_types(rows);
    CHECK(true);
  }

  // int32 and int64 are two shapes: the fill would read the late int64 as
  // an int32 slot, miss, and send a NULL without a word.
  {
    std::vector<params> rows{ params(std::int32_t(1)),
      params(std::int64_t(2)) };
    CHECK_THROWS(
      odbc::check_batch_column_types(rows), uniorm::backend::backend_error);
  }

  // Text after integers is the same trap across the widest type gap.
  {
    std::vector<params> rows{ params(std::int32_t(1), std::string("a")),
      params(std::int32_t(2), std::int32_t(3)) };
    CHECK_THROWS(
      odbc::check_batch_column_types(rows), uniorm::backend::backend_error);
  }

  // A column that never leaves NULL has no shape to disagree with.
  {
    std::vector<params> rows{ params(std::monostate{}),
      params(std::monostate{}) };
    odbc::check_batch_column_types(rows);
    CHECK(true);
  }
}
