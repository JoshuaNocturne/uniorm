#pragma once

// Private header: the libpq implementation of the backend contract. Not
// installed; consumers interact with uniorm::backend interfaces only.

#include <memory>
#include <string>
#include <string_view>

#include <uniorm/backend/backend.hpp>

#include "connection.hpp"
#include "schema_catalog.hpp"

namespace uniorm::libpq {

// The session as the contract sees it. Schema introspection is answered from
// pg_catalog by metadata_, which borrows the same session and reads inside its
// open transaction.
class UNIORM_LIBPQ_API pq_connection : public backend::connection_iface {
public:
  pq_connection();

  void open(std::string_view connection_string) override;
  void open(connection_spec const& spec) override;
  void close() override;
  bool is_open() const noexcept override;
  void set_autocommit(bool enabled) override;
  void commit() override;
  void rollback() override;
  backend::capabilities caps() const noexcept override;
  std::string dbms_name() const override;
  std::string server_version() const override;
  std::unique_ptr<backend::statement_iface> create_statement() override;
  schema_meta* schema() noexcept override;
  void* native_handle() noexcept override;

private:
  libpq::connection conn_;
  pq_schema_meta metadata_;
};

}  // namespace uniorm::libpq
