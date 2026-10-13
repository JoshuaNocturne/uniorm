#pragma once

// Materialisation of a registered entity straight from a result set: one
// binding per mapped column, each positioned by the result column that carries
// its label. Lives apart from the builders because `orm::query<T>(sql)` reads
// through it too.

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/detail/projection.hpp>
#include <uniorm/mapping/registry.hpp>

namespace uniorm::detail {

// Binds all mapped columns directly onto the fields of one entity instance.
// Each field binds at the result column labelled with its mapped name, so a
// SELECT whose column order differs from meta.columns still materializes
// into the right fields.
template <class T>
class entity_binding {
public:
  explicit entity_binding(entity_meta const& meta) : meta_(&meta) {
    bindings_.reserve(meta.columns.size());
    for (auto const& column : meta.columns) {
      bindings_.push_back(column.make_binding(&proto_));
    }
  }

  void set_row_array_size(std::size_t size) {
    row_array_size_ = size;
  }

  void bind(backend::statement_iface& stmt) {
    auto const result = stmt.column_meta();
    for (std::size_t index = 0; index < bindings_.size(); ++index) {
      bindings_[index]->set_row_array_size(row_array_size_);
      bindings_[index]->bind(
        stmt, meta_->result_ordinal(index, result), result);
    }
  }

  T take(std::size_t row_index) {
    for (auto& binding : bindings_) {
      binding->finalize(row_index, &proto_);
    }
    return std::move(proto_);
  }

  // Materialize one row directly into `dest` (vector storage). Bindings
  // write at captured member offsets, so no prototype round trip or
  // per-row move of small (SSO) strings is needed.
  void fill_into(T& dest, std::size_t row_index) {
    for (auto& binding : bindings_) {
      binding->finalize(row_index, &dest);
    }
  }

private:
  entity_meta const* meta_;
  T proto_{};
  std::size_t row_array_size_ = 1;
  std::vector<std::unique_ptr<field_binding>> bindings_;
};

}  // namespace uniorm::detail
