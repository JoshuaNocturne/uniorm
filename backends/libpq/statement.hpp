#pragma once

// Private header: the libpq statement, which is the backend contract's
// statement implemented over one session. Not installed.
//
// The core binds result columns after it has executed the statement, and it
// hands each column an array of caller slots to fill. libpq answers a query
// with the whole result set in memory, so a statement here needs neither a
// describe pass nor a cursor: the columns are readable as soon as the execute
// returns, and a fetch moves a row window over what the server already sent
// and writes that window into the caller's slots.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <uniorm/backend/backend.hpp>

#include "connection.hpp"
#include "handles.hpp"

namespace uniorm::libpq {

// One caller binding, and the select-list item it was asked for. The core
// binds in its own order, which need not be the server's, and a continuation
// read names the column rather than the arrival.
struct column_slot {
  backend::column_buffer target;
  std::size_t position = 0;
  bool bound = false;
};

class UNIORM_LIBPQ_API pq_statement : public backend::statement_iface {
public:
  explicit pq_statement(connection& conn);
  ~pq_statement() override;

  pq_statement(pq_statement const&) = delete;
  pq_statement& operator=(pq_statement const&) = delete;

  void prepare(std::string_view sql) override;
  void bind_parameter(std::size_t index, sql_value const& value) override;
  void bind_column(std::size_t index, backend::column_buffer const& buffer)
    override;
  void bind_batch_params(std::vector<params> const& rows) override;
  backend::batch_writer_iface& prepare_batch() override;
  void execute() override;
  bool fetch() override;
  std::size_t affected_rows() const override;
  std::vector<column_info> column_meta() const override;
  void set_row_array_size(std::size_t size) override;
  std::size_t rows_fetched() const override;
  std::size_t result_row_estimate() const override;
  void set_paramset_size(std::size_t size) override;
  std::string read_long_text(std::size_t column, std::size_t row) override;
  std::vector<std::byte> read_long_bytes(
    std::size_t column, std::size_t row) override;
  void reset() override;

private:
  // The session every call here reaches through. A statement that outlived it
  // would hand libpq a null handle, which is not something libpq reports.
  PGconn* session() const;

  // The text under this statement's own name, and the parameter types the
  // server inferred for it, which decide how each value is spelled.
  void prepare_under_name();
  // The same parse with the bound values left where they stand: a name the
  // server gave back on its own needs making again, not restaging.
  void make_ready();
  void read_parameter_types();
  // The rendered form of one parameter set: every value as the text libpq
  // carries to the server, and the pointer array that hands them over.
  void render(std::vector<sql_value> const& values);
  std::string render_one(sql_value const& value, unsigned type) const;
  PGresult* run();
  // One execute of what `render` staged, under a parse put back first if the
  // server no longer holds the name this statement answers to.
  detail::result_holder run_ready();
  void apply(detail::result_holder&& next);
  void run_batch();
  void declare_shape();
  std::size_t affected_of(PGresult* answer) const;
  void materialize(std::size_t rows);
  void fill_cell(column_slot& slot, std::size_t row);
  void put_text(
    column_slot& slot, std::size_t row, char const* data, std::size_t length);
  void put_bytes(
    column_slot& slot, std::size_t row, char const* data, std::size_t length);
  void put_integer(column_slot& slot, std::size_t row, char const* data) const;
  void put_double(column_slot& slot, std::size_t row, char const* data) const;
  void put_timestamp(column_slot& slot, std::size_t row, char const* data,
    unsigned native) const;
  char const* cell(
    std::size_t column, std::size_t row, std::size_t& length) const;
  column_slot& slot_for(std::size_t column);

  connection& conn_;
  // A name of its own, so two cached statements over the same text cannot
  // collide, and the server keeps one parse per statement instead of one per
  // execute.
  std::string statement_name_;
  std::string rewritten_;
  std::size_t placeholder_count_ = 0;
  std::vector<unsigned> param_types_;
  std::vector<sql_value> values_;
  std::vector<params> batch_;
  std::vector<std::string> param_text_;
  std::vector<char const*> param_pointers_;
  detail::result_holder result_;
  std::vector<column_slot> slots_;

  std::size_t row_array_size_ = 1;
  // The row the current block starts at, so a continuation read of a row in
  // the block can be turned back into a position in the result.
  std::size_t block_start_ = 0;
  std::size_t cursor_ = 0;
  std::size_t rows_fetched_ = 0;
  std::size_t affected_ = 0;
  bool prepared_ = false;
  bool query_ = false;
};

}  // namespace uniorm::libpq
