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

column_names::~column_names() = default;

column_names::column_names(column_names const&) = default;
column_names::column_names(column_names&&) noexcept = default;
column_names& column_names::operator=(column_names const&) = default;
column_names& column_names::operator=(column_names&&) noexcept = default;

struct row::impl {
  std::shared_ptr<column_names> names;
  std::vector<sql_value> values;

  impl() : names(std::make_shared<column_names>()) {}
  impl(std::vector<std::string> column_list, std::vector<sql_value> v)
    : names(std::make_shared<column_names>(std::move(column_list))),
      values(std::move(v)) {}
};

row::row() : impl_(new impl()) {}

row::row(std::vector<std::string> column_list, std::vector<sql_value> values)
  : impl_(new impl(std::move(column_list), std::move(values))) {}

row::~row() {
  destroy();
}

void row::destroy() noexcept {
  // Temporarily leak impl to confirm crash is in delete
  impl_ = nullptr;
}

row::row(row const& other)
  : impl_(new impl(*other.impl_)) {}

row::row(row&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

row& row::operator=(row const& other) {
  if (this != &other) {
    destroy();
    impl_ = new impl(*other.impl_);
  }
  return *this;
}

row& row::operator=(row&& other) noexcept {
  if (this != &other) {
    destroy();
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

sql_value const& row::at(std::string_view name) const {
  auto it = impl_->names->index.find(std::string(name));
  if (it == impl_->names->index.end()) {
    throw column_not_found("column not found: " + std::string(name));
  }
  return impl_->values[it->second];
}

sql_value const& row::at(std::size_t index) const {
  if (index >= impl_->values.size()) {
    throw column_not_found(
      "column index out of range: " + std::to_string(index));
  }
  return impl_->values[index];
}

bool row::is_null(std::string_view name) const {
  return uniorm::is_null(at(name));
}

bool row::is_null(std::size_t index) const {
  return uniorm::is_null(at(index));
}

std::size_t row::size() const noexcept {
  return impl_->values.size();
}

std::vector<std::string> const& row::names() const noexcept {
  return impl_->names->names;
}

}  // namespace uniorm
