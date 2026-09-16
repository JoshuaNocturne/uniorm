#pragma once

// Private header: the ODBC implementation of uniorm::schema_meta,
// built on the driver's catalog procedures. Not installed.

#include <string>
#include <string_view>
#include <vector>

#include <uniorm/schema.hpp>
#include "connection.hpp"

namespace uniorm::odbc {

struct odbc_schema_meta : schema_meta {
  explicit odbc_schema_meta(connection& conn) : conn_(conn) {}

  std::string database_name() override;
  std::vector<table_row> tables(
    std::string_view catalog, std::string_view schema) override;
  std::vector<column_row> table_columns(table_ref const&) override;
  std::vector<std::string> primary_key(table_ref const&) override;
  std::vector<foreign_key_row> foreign_keys(table_ref const&) override;
  std::vector<index_row> indexes(table_ref const&) override;

  connection& conn_;
};

}  // namespace uniorm::odbc
