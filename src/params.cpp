#include "uniorm/params.hpp"

#include <algorithm>
#include <vector>

#include <sql.h>
#include <sqlext.h>

#include "uniorm/detail/time.hpp"
#include "uniorm/odbc/error.hpp"
#include "uniorm/odbc/statement.hpp"

namespace uniorm {

detail::param_staging params::bind(odbc::statement& stmt) const {
  detail::param_staging staging;
  staging.slots.resize(values_.size());

  for (std::size_t i = 0; i < values_.size(); ++i) {
    sql_value const& v = values_[i];
    detail::param_staging::slot& slot = staging.slots[i];
    auto index = static_cast<SQLUSMALLINT>(i + 1);

    if (std::holds_alternative<std::monostate>(v)) {
      slot.indicator = SQL_NULL_DATA;
      stmt.bind_parameter(
        index, SQL_C_CHAR, SQL_VARCHAR, nullptr, 0, &slot.indicator, 1);
    } else if (auto* p = std::get_if<bool>(&v)) {
      slot.bit = *p ? 1 : 0;
      slot.indicator = sizeof(slot.bit);
      stmt.bind_parameter(index, SQL_C_BIT, SQL_BIT, &slot.bit,
        sizeof(slot.bit), &slot.indicator);
    } else if (auto* p = std::get_if<std::int16_t>(&v)) {
      slot.indicator = sizeof(*p);
      stmt.bind_parameter(index, SQL_C_SSHORT, SQL_SMALLINT,
        const_cast<std::int16_t*>(p), sizeof(*p), &slot.indicator);
    } else if (auto* p = std::get_if<std::int32_t>(&v)) {
      slot.indicator = sizeof(*p);
      stmt.bind_parameter(index, SQL_C_SLONG, SQL_INTEGER,
        const_cast<std::int32_t*>(p), sizeof(*p), &slot.indicator);
    } else if (auto* p = std::get_if<std::int64_t>(&v)) {
      slot.indicator = sizeof(*p);
      stmt.bind_parameter(index, SQL_C_SBIGINT, SQL_BIGINT,
        const_cast<std::int64_t*>(p), sizeof(*p), &slot.indicator);
    } else if (auto* p = std::get_if<double>(&v)) {
      slot.indicator = sizeof(*p);
      stmt.bind_parameter(index, SQL_C_DOUBLE, SQL_DOUBLE,
        const_cast<double*>(p), sizeof(*p), &slot.indicator);
    } else if (auto* p = std::get_if<std::string>(&v)) {
      slot.indicator = static_cast<SQLLEN>(p->size());
      stmt.bind_parameter(index, SQL_C_CHAR, SQL_VARCHAR,
        const_cast<char*>(p->data()), static_cast<SQLLEN>(p->size()),
        &slot.indicator, std::max<SQLULEN>(p->size(), 1));
    } else if (auto* p = std::get_if<std::vector<std::byte>>(&v)) {
      slot.indicator = static_cast<SQLLEN>(p->size());
      stmt.bind_parameter(index, SQL_C_BINARY, SQL_VARBINARY,
        const_cast<std::byte*>(p->data()), static_cast<SQLLEN>(p->size()),
        &slot.indicator, std::max<SQLULEN>(p->size(), 1));
    } else if (auto* p = std::get_if<timestamp>(&v)) {
      auto parts = detail::break_timestamp(*p);
      slot.ts.year = static_cast<SQLSMALLINT>(parts.year);
      slot.ts.month = static_cast<SQLUSMALLINT>(parts.month);
      slot.ts.day = static_cast<SQLUSMALLINT>(parts.day);
      slot.ts.hour = static_cast<SQLUSMALLINT>(parts.hour);
      slot.ts.minute = static_cast<SQLUSMALLINT>(parts.minute);
      slot.ts.second = static_cast<SQLUSMALLINT>(parts.second);
      slot.ts.fraction = static_cast<SQLUINTEGER>(parts.fraction_ns);
      slot.indicator = sizeof(slot.ts);
      stmt.bind_parameter(index, SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP,
        &slot.ts, sizeof(slot.ts), &slot.indicator, 26, 6);
    }
  }
  return staging;
}

}  // namespace uniorm
