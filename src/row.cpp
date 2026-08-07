#include "uniorm/row.hpp"

#include <utility>

namespace uniorm {

row::row(std::shared_ptr<std::vector<std::string>> names,
  std::vector<sql_value> values)
  : names_(std::move(names)), values_(std::move(values)) {}

sql_value const& row::at(std::string_view name) const {
  for (std::size_t i = 0; i < names_->size(); ++i) {
    if ((*names_)[i] == name) {
      return values_[i];
    }
  }
  throw column_not_found("column not found: " + std::string(name));
}

sql_value const& row::at(std::size_t index) const {
  if (index >= values_.size()) {
    throw column_not_found(
      "column index out of range: " + std::to_string(index));
  }
  return values_[index];
}

}  // namespace uniorm
