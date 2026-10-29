#pragma once

// Private header: the libpq session. Not installed.

#include <string>
#include <string_view>

#include <libpq-fe.h>

#include "export.hpp"

namespace uniorm::libpq {

// The word list libpq is handed for a target given as one string after the
// scheme. A tail that is already one of libpq's own grammars goes to it whole,
// and a bare host has the scheme put back on it. An empty tail names no server
// at all, and comes back empty so the attempt is refused before a connection is
// opened to a host nobody asked for.
UNIORM_LIBPQ_API std::string conninfo_of(std::string_view tail);

// The same list from the four pieces: the target is
// "[host[:port]][/dbname]" -- or a word list of its own, in which case only
// the two pieces it cannot carry are added -- and an empty user or password is
// the server's default rather than a blank one.
UNIORM_LIBPQ_API std::string conninfo_of(std::string_view target,
  std::string_view user, std::string_view password);

// One connected session. libpq keeps no parent handle to share, so this owns
// the PGconn outright and the statements above it borrow it. Move-only: a
// session that closes twice would free the same handle twice.
class UNIORM_LIBPQ_API connection {
public:
  connection() = default;
  ~connection();

  connection(connection&& other) noexcept;
  connection& operator=(connection&& other) noexcept;

  connection(connection const&) = delete;
  connection& operator=(connection const&) = delete;

  // What follows "postgresql://", and the four pieces, each taken to libpq
  // through the spelling named above it.
  void open(std::string_view tail);
  void open(std::string_view target, std::string_view user,
    std::string_view password);
  void close();
  bool is_open() const noexcept;

  // libpq has no autocommit attribute to set: a statement outside a
  // transaction runs in one of its own, so the manual mode is a BEGIN issued
  // before the first statement that asks for it.
  void set_autocommit(bool enabled);
  bool autocommit() const noexcept {
    return autocommit_;
  }
  void open_transaction();
  void commit();
  void rollback();

  PGconn* native() const noexcept {
    return conn_;
  }

private:
  void connect(std::string const& conninfo);
  void run_transaction_command(
    char const* sql, std::string const& context);

  PGconn* conn_ = nullptr;
  bool autocommit_ = true;
  bool transaction_open_ = false;
};

}  // namespace uniorm::libpq
