#include "uniorm/transaction.hpp"

#include <uniorm/connection.hpp>

namespace uniorm {

transaction::transaction(connection& conn) : conn_(&conn) {
  // Only a transaction that switched autocommit off may switch it back on: a
  // caller who put the connection in manual mode on purpose keeps it.
  owns_mode_ = conn.autocommit();
  if (owns_mode_) {
    conn_->set_autocommit(false);
  }
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
  : conn_(other.conn_), active_(other.active_), owns_mode_(other.owns_mode_) {
  other.conn_ = nullptr;
  other.active_ = false;
  other.owns_mode_ = false;
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
    owns_mode_ = other.owns_mode_;
    other.conn_ = nullptr;
    other.active_ = false;
    other.owns_mode_ = false;
  }
  return *this;
}

void transaction::commit() {
  if (!active_) {
    return;
  }
  conn_->commit();
  if (owns_mode_) {
    conn_->set_autocommit(true);
  }
  active_ = false;
}

void transaction::rollback() {
  if (!active_) {
    return;
  }
  conn_->rollback();
  if (owns_mode_) {
    conn_->set_autocommit(true);
  }
  active_ = false;
}

}  // namespace uniorm
