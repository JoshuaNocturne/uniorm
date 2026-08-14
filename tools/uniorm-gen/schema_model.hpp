#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uniorm::gen {

// Database-neutral schema snapshot extracted from ODBC metadata.

struct column_model {
  std::string name;
  std::string type_name;  // as reported by the driver (TYPE_NAME)
  int data_type = 0;  // native SQL_* type code (DATA_TYPE)
  std::int32_t size = 0;
  std::int16_t decimals = 0;
  bool nullable = false;
  bool primary_key = false;
  std::optional<std::string> default_value;
};

// This table references pk_table; each pair maps a local column to the
// referenced primary-key column.
struct fk_model {
  std::string pk_table;
  std::vector<std::pair<std::string, std::string>> columns;
};

struct index_model {
  std::string name;
  std::vector<std::string> columns;
  bool unique = false;
};

struct table_model {
  std::string catalog;
  std::string schema;
  std::string name;
  std::vector<column_model> columns;
  std::vector<fk_model> foreign_keys;
  std::vector<index_model> indexes;
};

struct schema_model {
  std::string name;  // drives namespace, header name, register function
  std::vector<table_model> tables;
};

}  // namespace uniorm::gen
