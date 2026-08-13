#pragma once

// Typed projection binding: binds result columns by ordinal onto the fields
// of an aggregate struct discovered via pfr-lite.

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <sql.h>
#include <sqlext.h>

#include "../error.hpp"
#include "../odbc/error.hpp"
#include "../odbc/statement.hpp"
#include "../value.hpp"
#include "pfr.hpp"
#include "time.hpp"
#include "traits.hpp"

namespace uniorm::detail {

class field_binding {
public:
  virtual ~field_binding() = default;

  virtual void bind(odbc::statement& stmt, SQLUSMALLINT column) = 0;
  // Copy staging into the target field; assumes data is non-NULL.
  virtual void materialize() {}

  virtual void finalize() {
    if (ind_ == SQL_NULL_DATA) {
      throw type_mismatch("NULL value for non-optional projection field");
    }
    materialize();
  }

  SQLLEN indicator() const noexcept {
    return ind_;
  }

protected:
  SQLLEN ind_ = 0;
};

namespace {

// Some drivers (e.g. MariaDB Connector/ODBC) return the FULL remaining
// value from SQLGetData after a truncated bound-column fetch, not just the
// tail. Read into a fresh string so callers can replace the partial bound
// buffer instead of appending.
std::string get_data_char(odbc::statement& stmt, SQLUSMALLINT column) {
  std::string out;
  for (;;) {
    char chunk[4096];
    chunk[0] = '\0';
    SQLLEN chunk_ind = 0;
    SQLRETURN rc = SQLGetData(
      stmt.native(), column, SQL_C_CHAR, chunk, sizeof(chunk), &chunk_ind);
    if (rc == SQL_NO_DATA || chunk_ind == SQL_NULL_DATA) {
      return out;
    }
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt.native(), "read long character data");
    out.append(chunk, std::strlen(chunk));
    // SQL_SUCCESS_WITH_INFO (01004) means the chunk was truncated; keep going.
    if (rc != SQL_SUCCESS_WITH_INFO) {
      return out;
    }
  }
}

std::vector<std::byte> get_data_binary(
  odbc::statement& stmt, SQLUSMALLINT column) {
  std::vector<std::byte> out;
  for (;;) {
    std::byte chunk[4096];
    SQLLEN chunk_ind = 0;
    SQLRETURN rc = SQLGetData(
      stmt.native(), column, SQL_C_BINARY, chunk, sizeof(chunk), &chunk_ind);
    if (rc == SQL_NO_DATA || chunk_ind == SQL_NULL_DATA) {
      return out;
    }
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt.native(), "read long binary data");
    std::size_t take;
    if (rc == SQL_SUCCESS_WITH_INFO || chunk_ind == SQL_NO_TOTAL) {
      take = sizeof(chunk);  // truncated: buffer is full
    } else {
      take = std::min<SQLLEN>(chunk_ind, static_cast<SQLLEN>(sizeof(chunk)));
    }
    out.insert(out.end(), chunk, chunk + take);
    if (rc != SQL_SUCCESS_WITH_INFO) {
      return out;
    }
  }
}

}  // namespace

template <class T>
class direct_binding : public field_binding {
public:
  explicit direct_binding(T& target) : target_(target) {}

  void bind(odbc::statement& stmt, SQLUSMALLINT column) override {
    SQLRETURN rc =
      SQLBindCol(stmt.native(), column, c_type(), &target_, sizeof(T), &ind_);
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt.native(), "bind projection column");
  }

private:
  static SQLSMALLINT c_type() {
    if constexpr (std::is_same_v<T, bool>)
      return SQL_C_BIT;
    else if constexpr (std::is_same_v<T, std::int8_t>)
      return SQL_C_STINYINT;
    else if constexpr (std::is_same_v<T, std::int16_t>)
      return SQL_C_SSHORT;
    else if constexpr (std::is_same_v<T, std::int32_t>)
      return SQL_C_SLONG;
    else if constexpr (std::is_same_v<T, std::int64_t>)
      return SQL_C_SBIGINT;
    else if constexpr (std::is_same_v<T, float>)
      return SQL_C_FLOAT;
    else if constexpr (std::is_same_v<T, double>)
      return SQL_C_DOUBLE;
    else
      static_assert(
        std::is_same_v<T, T> && false, "unsupported direct binding type");
  }

  T& target_;
};

class string_binding : public field_binding {
public:
  explicit string_binding(std::string& target) : target_(target) {
    buf_.resize(256);
  }

  void bind(odbc::statement& stmt, SQLUSMALLINT column) override {
    stmt_ = &stmt;
    column_ = column;
    SQLRETURN rc = SQLBindCol(stmt.native(), column, SQL_C_CHAR, buf_.data(),
      static_cast<SQLLEN>(buf_.size()), &ind_);
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt.native(), "bind projection column");
  }

  void materialize() override {
    if (ind_ == SQL_NO_TOTAL || ind_ > static_cast<SQLLEN>(buf_.size()) - 1) {
      target_ = get_data_char(*stmt_, column_);  // full value, replaces buffer
    } else {
      target_.assign(buf_.data(), static_cast<std::size_t>(ind_));
    }
  }

private:
  std::string& target_;
  std::vector<char> buf_;
  odbc::statement* stmt_ = nullptr;
  SQLUSMALLINT column_ = 0;
};

class binary_binding : public field_binding {
public:
  explicit binary_binding(std::vector<std::byte>& target) : target_(target) {
    buf_.resize(256);
  }

  void bind(odbc::statement& stmt, SQLUSMALLINT column) override {
    stmt_ = &stmt;
    column_ = column;
    SQLRETURN rc = SQLBindCol(stmt.native(), column, SQL_C_BINARY, buf_.data(),
      static_cast<SQLLEN>(buf_.size()), &ind_);
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt.native(), "bind projection column");
  }

  void materialize() override {
    bool truncated =
      ind_ == SQL_NO_TOTAL || ind_ > static_cast<SQLLEN>(buf_.size());
    if (truncated) {
      target_ =
        get_data_binary(*stmt_, column_);  // full value, replaces buffer
    } else {
      target_.assign(
        buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(ind_));
    }
  }

private:
  std::vector<std::byte>& target_;
  std::vector<std::byte> buf_;
  odbc::statement* stmt_ = nullptr;
  SQLUSMALLINT column_ = 0;
};

class timestamp_binding : public field_binding {
public:
  explicit timestamp_binding(timestamp& target) : target_(target) {}

  void bind(odbc::statement& stmt, SQLUSMALLINT column) override {
    SQLRETURN rc = SQLBindCol(stmt.native(), column, SQL_C_TYPE_TIMESTAMP,
      &staging_, sizeof(staging_), &ind_);
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt.native(), "bind projection column");
  }

  void materialize() override {
    target_ = make_timestamp(staging_.year, staging_.month, staging_.day,
      staging_.hour, staging_.minute, staging_.second, staging_.fraction);
  }

private:
  timestamp& target_;
  SQL_TIMESTAMP_STRUCT staging_{};
};

template <class T>
std::unique_ptr<field_binding> make_field_binding(T& field);

template <class T>
class optional_binding : public field_binding {
public:
  explicit optional_binding(std::optional<T>& target) : target_(target) {}

  void bind(odbc::statement& stmt, SQLUSMALLINT column) override {
    inner_ = make_field_binding(storage_);
    inner_->bind(stmt, column);
  }

  void finalize() override {
    if (inner_->indicator() == SQL_NULL_DATA) {
      target_.reset();
    } else {
      inner_->materialize();
      target_ = std::move(storage_);
    }
  }

private:
  std::optional<T>& target_;
  T storage_{};
  std::unique_ptr<field_binding> inner_;
};

template <class T>
concept directly_bindable =
  std::is_same_v<T, bool> || std::is_same_v<T, std::int8_t> ||
  std::is_same_v<T, std::int16_t> || std::is_same_v<T, std::int32_t> ||
  std::is_same_v<T, std::int64_t> || std::is_same_v<T, float> ||
  std::is_same_v<T, double>;

template <class T>
std::unique_ptr<field_binding> make_field_binding(T& field) {
  using U = std::remove_cvref_t<T>;
  if constexpr (is_optional_v<U>) {
    return std::make_unique<optional_binding<typename U::value_type>>(field);
  } else if constexpr (std::is_same_v<U, std::string>) {
    return std::make_unique<string_binding>(field);
  } else if constexpr (std::is_same_v<U, std::vector<std::byte>>) {
    return std::make_unique<binary_binding>(field);
  } else if constexpr (std::is_same_v<U, timestamp>) {
    return std::make_unique<timestamp_binding>(field);
  } else if constexpr (directly_bindable<U>) {
    return std::make_unique<direct_binding<U>>(field);
  } else {
    static_assert(std::is_same_v<U, U> && false,
      "unsupported projection field type: use a supported sql type, "
      "std::optional thereof, or a converter-backed entity mapping");
  }
}

template <class T>
class projection {
public:
  void bind(odbc::statement& stmt) {
    SQLUSMALLINT column = 0;
    for_each_field(proto_, [&](auto& field) {
      auto binding = make_field_binding(field);
      binding->bind(stmt, static_cast<SQLUSMALLINT>(column + 1));
      ++column;
      bindings_.push_back(std::move(binding));
    });
  }

  // Moves the assembled row out. Call exactly once per successful fetch():
  // proto_ is left moved-from and every field is rewritten by finalize() on
  // the next fetch, so the moved-from state is never observed by bindings.
  T take() {
    for (auto& binding : bindings_) {
      binding->finalize();
    }
    return std::move(proto_);
  }

private:
  T proto_{};
  std::vector<std::unique_ptr<field_binding>> bindings_;
};

}  // namespace uniorm::detail
