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
// cannot bind; a converter override is emitted as written.
generated_output generate_header(
  schema_model const& model, gen_config const& cfg);

}  // namespace uniorm::gen
