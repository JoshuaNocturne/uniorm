#pragma once

// Private header: the OCI implementation of the backend contract. Not
// installed; consumers interact with uniorm::backend interfaces only.

#include <memory>
#include <string>
#include <string_view>

#include <uniorm/backend/backend.hpp>

#include "connection.hpp"

namespace uniorm::oracle_oci {

// The session as the contract sees it. The schema reads stay at the contract's
// default of nullptr: saying introspection is unavailable beats answering from
// a catalog this backend cannot be tested against.
class oci_connection : public backend::connection_iface {
public:
  void open(std::string_view connection_string) override;
  void open(connection_spec const& spec) override;
  void close() override;
  bool is_open() const noexcept override;
  void set_autocommit(bool enabled) override;
  void commit() override;
  void rollback() override;
  backend::capabilities caps() const noexcept override;
  std::string dbms_name() const override;
  std::unique_ptr<backend::statement_iface> create_statement() override;
  void* native_handle() noexcept override;

private:
  oracle_oci::connection conn_;
};

}  // namespace uniorm::oracle_oci
