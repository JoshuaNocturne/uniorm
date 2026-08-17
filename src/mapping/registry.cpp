#include "uniorm/mapping/registry.hpp"

#include <unordered_map>
#include <utility>

#include "uniorm/backend/backend.hpp"
#include "uniorm/connection.hpp"

namespace uniorm {

std::string const& entity_meta::column_name(member_key const& key) const {
  for (auto const& c : columns) {
    if (c.key == key) {
      return c.column;
    }
  }
  throw mapping_error("member is not mapped to a column");
}

void entity_meta::populate(void* obj, row const& r) const {
  for (auto const& c : columns) {
    c.write(obj, r.at(c.column));
  }
}

namespace {

struct schema_column {
  bool nullable = false;
};

std::unordered_map<std::string, schema_column> load_table_schema(
  backend::schema_metadata& md, std::string const& table) {
  std::unordered_map<std::string, schema_column> schema;
  for (auto const& c : md.table_columns(table)) {
    schema[c.name] = schema_column{c.nullable};
  }
  return schema;
}

}  // namespace

void orm::validate(connection& conn, validation_mode mode) {
  auto* md = conn.extension<backend::schema_metadata>();
  if (md == nullptr) {
    throw mapping_error(
      "schema validation requires a backend that exposes schema metadata");
  }
  for (auto const& [type, meta] : entities_) {
    auto schema = load_table_schema(*md, meta.table);
    if (schema.empty()) {
      throw mapping_error("table not found: " + meta.table);
    }
    for (auto const& c : meta.columns) {
      auto it = schema.find(c.column);
      if (it == schema.end()) {
        throw mapping_error(
          "column not found in table " + meta.table + ": " + c.column);
      }
      if (mode == validation_mode::strict && it->second.nullable &&
          !c.nullable) {
        throw mapping_error("column " + meta.table + "." + c.column +
                            " is nullable but the mapped member is not "
                            "std::optional");
      }
    }
  }
}

}  // namespace uniorm
