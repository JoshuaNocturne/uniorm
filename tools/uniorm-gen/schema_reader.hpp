#pragma once

#include <optional>
#include <string>
#include <vector>

#include "schema_model.hpp"

namespace uniorm::odbc {
class connection;
}

namespace uniorm::gen {

struct read_options {
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::vector<std::string> tables;  // empty = every base table
};

// Extracts the schema of base tables (SQLTables/SQLColumns/SQLPrimaryKeys/
// SQLForeignKeys/SQLStatistics) through the ODBC metadata API. Warnings
// (e.g. case-insensitive table filter fallback) are appended to *warnings
// when it is non-null. Throws odbc::odbc_error on driver failures.
schema_model read_schema(odbc::connection& dbc, read_options const& opts,
  std::vector<std::string>* warnings = nullptr);

// SQLGetInfo(SQL_DATABASE_NAME), sanitized for use as a unit name; empty
// when the driver does not report one.
std::string database_name(odbc::connection& dbc);

}  // namespace uniorm::gen
