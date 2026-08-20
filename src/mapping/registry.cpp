#include "uniorm/mapping/registry.hpp"

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

}  // namespace uniorm
