#pragma once

// Private header: the libpq implementation of the backend contract. Not
// installed; consumers interact with uniorm::backend interfaces only.

#include <memory>
#include <string>
#include <string_view>

#include <uniorm/backend/backend.hpp>

#include "connection.hpp"

namespace uniorm::libpq {

// The session as the contract sees it. The schema reads stay at the contract's
// default of nullptr: the pg_catalog queries behind them are a second body of
// work, and a caller who asks is told that introspection is unavailable rather
// than answered from a catalog read that does not exist yet.
class UNIORM_LIBPQ_API pq_connection : public backend::connection_iface {
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
  libpq::connection conn_;
};

}  // namespace uniorm::libpq
