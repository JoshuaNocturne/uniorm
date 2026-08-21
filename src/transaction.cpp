#include "uniorm/transaction.hpp"

#include <uniorm/detail/connection.hpp>

namespace uniorm {

transaction::transaction(connection& conn) : conn_(&conn) {
  conn_->set_autocommit(false);
  active_ = true;
}

transaction::~transaction() {
  if (active_) {
    try {
      rollback();
    } catch (...) {
      // destruction must not throw; the connection will be closed or
      // reset by the caller if needed
    }
  }
}

transaction::transaction(transaction&& other) noexcept
  : conn_(other.conn_), active_(other.active_) {
  other.conn_ = nullptr;
  other.active_ = false;
}

transaction& transaction::operator=(transaction&& other) noexcept {
  if (this != &other) {
    if (active_) {
      try {
        rollback();
      } catch (...) {
      }
    }
    conn_ = other.conn_;
    active_ = other.active_;
    other.conn_ = nullptr;
    other.active_ = false;
  }
  return *this;
}

void transaction::commit() {
  if (!active_) {
    return;
  }
  conn_->commit();
  conn_->set_autocommit(true);
  active_ = false;
}

void transaction::rollback() {
  if (!active_) {
    return;
  }
  conn_->rollback();
  conn_->set_autocommit(true);
  active_ = false;
}

}  // namespace uniorm
