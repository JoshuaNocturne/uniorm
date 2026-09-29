#include "uniorm/row.hpp"

#include <utility>

namespace uniorm {

column_names::column_names() = default;

column_names::column_names(std::vector<std::string> column_list)
  : names(std::move(column_list)) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    index.emplace(names[i], i);
  }
}

column_names::~column_names() {
  names.clear();
  index.clear();
}

row::row(std::shared_ptr<column_names> names, std::vector<sql_value> values)
  : names_(std::move(names)), values_(std::move(values)) {}

row::~row() {
  names_.reset();
  values_.clear();
}

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
