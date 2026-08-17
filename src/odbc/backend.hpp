#pragma once

// Private header: the ODBC implementation of the backend contract.
// Not installed; consumers interact with uniorm::backend interfaces only.

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
  void execute() override;
  bool fetch() override;
  std::size_t affected_rows() const override;
  std::vector<column_info> column_meta() const override;
  std::string read_long_text(std::size_t column) override;
  std::vector<std::byte> read_long_bytes(std::size_t column) override;
  void reset() override;

private:
  odbc::statement stmt_;
  struct param_slot;
  std::vector<param_slot> slots_;
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
