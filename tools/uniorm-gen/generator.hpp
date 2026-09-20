#pragma once

#include <string>
#include <vector>

#include "config.hpp"
#include "schema_model.hpp"

namespace uniorm::gen {

struct generated_output {
  std::string text;
  std::vector<std::string> warnings;
};

// Renders <name>_schema.hpp content from a schema snapshot plus overrides.
// model.name must already be a valid C++ identifier (namespace/unit name).
// Throws config_error when a cpp_type override names a type the registry
// cannot bind, or when two tables are given one class name; a converter
// override is emitted as written.
generated_output generate_header(
  schema_model const& model, gen_config const& cfg);

// Checks every config section against the catalog before anything is written:
// a section that matches no table, or a column block that matches no column of
// a table this run read, is an override that would otherwise be ignored in
// silence. catalog_tables is every table the catalog listed, including the ones
// a --tables selection left out. Throws config_error naming each offender.
void check_config(gen_config const& cfg, schema_model const& model,
  std::vector<std::string> const& catalog_tables);

}  // namespace uniorm::gen
