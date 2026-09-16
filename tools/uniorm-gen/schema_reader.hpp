#pragma once

#include <string>
#include <vector>

#include <uniorm/schema.hpp>
#include "schema_model.hpp"

namespace uniorm::gen {

struct read_options {
  // Empty leaves the catalog or schema up to what the connection sees.
  std::string catalog;
  std::string schema;
  std::vector<std::string> tables;  // empty = every table the backend lists
};

// Builds the generator's schema snapshot out of a backend's introspection.
// Which reads a backend performs is the backend's business; what to keep of
// them is decided here. Catalog oddities (a table that came back without
// columns) are appended to *warnings when it is non-null. Throws
// uniorm_error when a named table is missing.
schema_model read_schema(schema_meta& md,
  read_options const& opts, std::vector<std::string>* warnings = nullptr);

}  // namespace uniorm::gen
