#pragma once

#include <uniorm/export.hpp>

namespace uniorm {

class connection;

// RAII transaction scope: switches the connection out of autocommit on
// construction; uncommitted work is rolled back on destruction.
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
};

}  // namespace uniorm
