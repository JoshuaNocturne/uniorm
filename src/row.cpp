#include "uniorm/row.hpp"

#include <utility>

namespace uniorm {

row::row(std::shared_ptr<column_names> names, std::vector<sql_value> values)
  : names_(std::move(names)), values_(std::move(values)) {}

sql_value const& row::at(std::string_view name) const {
  auto it = names_->index.find(std::string(name));
  if (it == names_->index.end()) {
    throw column_not_found("column not found: " + std::string(name));
  }
  return values_[it->second];
}

sql_value const& row::at(std::size_t index) const {
  if (index >= values_.size()) {
    throw column_not_found(
      "column index out of range: " + std::to_string(index));
  }
  return values_[index];
}

}  // namespace uniorm
