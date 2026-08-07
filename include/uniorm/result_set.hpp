#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "export.hpp"
#include "row.hpp"
#include "types.hpp"

namespace uniorm {

namespace odbc {
class statement;
}

struct column_info {
  std::string name;
  sql_type type;
  std::size_t display_size;
  bool nullable;
};

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

  static result_set from_statement(odbc::statement stmt);

  std::unique_ptr<impl> impl_;
};

}  // namespace uniorm
