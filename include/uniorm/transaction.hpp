#pragma once

#include <uniorm/export.hpp>

namespace uniorm {

class connection;

// RAII transaction scope: switches the connection out of autocommit on
// construction unless the connection is already manual; uncommitted work is
// rolled back on destruction. It only restores the commit mode it changed.
class UNIORM_API transaction {
public:
  explicit transaction(connection& conn);
  ~transaction();

  transaction(transaction&&) noexcept;
  transaction& operator=(transaction&&) noexcept;

  transaction(transaction const&) = delete;
  transaction& operator=(transaction const&) = delete;

  void commit();
  void rollback();

  bool active() const noexcept {
    return active_;
  }

private:
  connection* conn_ = nullptr;
  bool active_ = false;
  // Whether this transaction is the one that turned autocommit off.
  bool owns_mode_ = false;
};

}  // namespace uniorm
