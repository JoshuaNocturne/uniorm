#pragma once

// Private header: the ODBC implementation of the backend contract.
// Not installed; consumers interact with uniorm::backend interfaces only.

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/odbc/connection.hpp>
#include <uniorm/odbc/statement.hpp>

namespace uniorm::odbc {

class backend_statement : public backend::statement_iface {
public:
  explicit backend_statement(odbc::connection& conn);

  void prepare(std::string_view sql) override;
  void bind_parameter(std::size_t index, sql_value const& value) override;
  void bind_column(
    std::size_t index, backend::column_buffer const& buffer) override;
  void bind_batch_params(std::vector<params> const& rows) override;
  backend::batch_writer_iface& prepare_batch() override;
  void reset_parameters() override;
  void execute() override;
  bool fetch() override;
  std::size_t affected_rows() const override;
  std::size_t result_row_estimate() const override;
  std::vector<column_info> column_meta() const override;
  void set_row_array_size(std::size_t size) override;
  std::size_t rows_fetched() const override;
  void set_paramset_size(std::size_t size) override;
  std::string read_long_text(std::size_t column) override;
  std::vector<std::byte> read_long_bytes(std::size_t column) override;
  void reset() override;

private:
  odbc::statement stmt_;
  struct param_slot;
  // deque, not vector: bind_parameter hands the driver pointers into
  // existing slots, and appending further slots must not relocate them.
  std::deque<param_slot> slots_;

  // Column buffers for batch parameter binding. Owned by the backend so
  // callers don't need to know the physical layout.
  struct batch_col {
    backend::buffer_type type = backend::buffer_type::chars;
    std::vector<unsigned char> bit_vals;
    std::vector<std::int16_t> i16_vals;
    std::vector<std::int32_t> i32_vals;
    std::vector<std::int64_t> i64_vals;
    std::vector<double> f64_vals;
    std::vector<backend::timestamp_parts> ts_vals;
    std::vector<char> var_buf;
    std::size_t stride = 0;
    std::vector<std::int64_t> indicators;
    std::size_t count = 0;
  };
  std::vector<batch_col> batch_cols_;

  // Direct-write batch writer. Owns no data itself — references batch_cols_.
  class odbc_batch_writer;
  std::unique_ptr<odbc_batch_writer> batch_writer_;
};

class backend_connection : public backend::connection_iface {
public:
  backend_connection();

  void open(std::string_view connection_string) override;
  void close() override;
  bool is_open() const noexcept override;
  void set_autocommit(bool enabled) override;
  void commit() override;
  void rollback() override;
  backend::capabilities caps() const noexcept override;
  std::string dbms_name() const override;
  std::unique_ptr<backend::statement_iface> create_statement() override;
  void* native_handle() noexcept override;
  void* extension(std::type_index id) noexcept override;

private:
  odbc::connection conn_;
  struct schema_metadata_impl;
  std::unique_ptr<schema_metadata_impl> metadata_;
};

}  // namespace uniorm::odbc
