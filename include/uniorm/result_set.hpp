#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include <uniorm/export.hpp>
#include <uniorm/row.hpp>
#include <uniorm/types.hpp>

namespace uniorm {

namespace backend {
struct statement_iface;
}

// Row-wise bound result set. Move-only. Created by connection::execute.
class UNIORM_API result_set {
public:
  ~result_set();
  result_set(result_set&&) noexcept;
  result_set& operator=(result_set&&) noexcept;

  result_set(result_set const&) = delete;
  result_set& operator=(result_set const&) = delete;

  bool next();  // fetch next row; false when exhausted
  row current();  // materialize the current row

  std::size_t column_count() const;
  column_info const& column(std::size_t index) const;

private:
  friend class connection;
  struct impl;
  explicit result_set(std::unique_ptr<impl> i);

  // release receives the statement when this result_set is destroyed
  // (statement cache check-in); it must not throw. May be empty.
  static result_set from_statement(
    std::unique_ptr<backend::statement_iface> stmt,
    std::function<void(std::unique_ptr<backend::statement_iface>)> release);

  std::unique_ptr<impl> impl_;
};

}  // namespace uniorm
