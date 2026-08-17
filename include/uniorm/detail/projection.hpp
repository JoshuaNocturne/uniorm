#pragma once

// Typed projection binding: binds result columns by ordinal onto the fields
// of an aggregate struct discovered via pfr-lite. All binding goes through
// the backend-neutral statement interface.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/error.hpp>
#include <uniorm/value.hpp>
#include <uniorm/detail/pfr.hpp>
#include <uniorm/detail/time.hpp>
#include <uniorm/detail/traits.hpp>

namespace uniorm::detail {

class field_binding {
public:
  virtual ~field_binding() = default;

  virtual void bind(backend::statement_iface& stmt, std::size_t column) = 0;
  // Copy staging into the target field; assumes data is non-NULL.
  virtual void materialize() {}

  virtual void finalize() {
    if (ind_ == backend::null_indicator) {
      throw type_mismatch("NULL value for non-optional projection field");
    }
    materialize();
  }

  std::int64_t indicator() const noexcept {
    return ind_;
  }

protected:
  std::int64_t ind_ = 0;
};

template <class T>
class direct_binding : public field_binding {
public:
  explicit direct_binding(T& target) : target_(target) {}

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    stmt.bind_column(column,
      {buffer_kind(), &target_, sizeof(T), &ind_});
  }

private:
  static backend::buffer_type buffer_kind() {
    if constexpr (std::is_same_v<T, bool>)
      return backend::buffer_type::bit;
    else if constexpr (std::is_same_v<T, std::int8_t>)
      return backend::buffer_type::int8;
    else if constexpr (std::is_same_v<T, std::int16_t>)
      return backend::buffer_type::int16;
    else if constexpr (std::is_same_v<T, std::int32_t>)
      return backend::buffer_type::int32;
    else if constexpr (std::is_same_v<T, std::int64_t>)
      return backend::buffer_type::int64;
    else if constexpr (std::is_same_v<T, float>)
      return backend::buffer_type::float32;
    else if constexpr (std::is_same_v<T, double>)
      return backend::buffer_type::float64;
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

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    stmt_ = &stmt;
    column_ = column;
    stmt.bind_column(column,
      {backend::buffer_type::chars, buf_.data(), buf_.size(), &ind_});
  }

  void materialize() override {
    if (ind_ == backend::no_total ||
        ind_ > static_cast<std::int64_t>(buf_.size()) - 1) {
      // Full value from the backend; replaces the partial bound buffer.
      target_ = stmt_->read_long_text(column_);
    } else {
      target_.assign(buf_.data(), static_cast<std::size_t>(ind_));
    }
  }

private:
  std::string& target_;
  std::vector<char> buf_;
  backend::statement_iface* stmt_ = nullptr;
  std::size_t column_ = 0;
};

class binary_binding : public field_binding {
public:
  explicit binary_binding(std::vector<std::byte>& target) : target_(target) {
    buf_.resize(256);
  }

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    stmt_ = &stmt;
    column_ = column;
    stmt.bind_column(column,
      {backend::buffer_type::bytes, buf_.data(), buf_.size(), &ind_});
  }

  void materialize() override {
    bool truncated = ind_ == backend::no_total ||
                     ind_ > static_cast<std::int64_t>(buf_.size());
    if (truncated) {
      target_ = stmt_->read_long_bytes(column_);  // full value, replaces
    } else {
      target_.assign(
        buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(ind_));
    }
  }

private:
  std::vector<std::byte>& target_;
  std::vector<std::byte> buf_;
  backend::statement_iface* stmt_ = nullptr;
  std::size_t column_ = 0;
};

class timestamp_binding : public field_binding {
public:
  explicit timestamp_binding(timestamp& target) : target_(target) {}

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    stmt.bind_column(column, {backend::buffer_type::timestamp_parts,
                           &staging_, sizeof(staging_), &ind_});
  }

  void materialize() override {
    target_ = make_timestamp(staging_.year, staging_.month, staging_.day,
      staging_.hour, staging_.minute, staging_.second,
      staging_.fraction_ns);
  }

private:
  timestamp& target_;
  backend::timestamp_parts staging_{};
};

template <class T>
std::unique_ptr<field_binding> make_field_binding(T& field);

template <class T>
class optional_binding : public field_binding {
public:
  explicit optional_binding(std::optional<T>& target) : target_(target) {}

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    inner_ = make_field_binding(storage_);
    inner_->bind(stmt, column);
  }

  void finalize() override {
    if (inner_->indicator() == backend::null_indicator) {
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
  void bind(backend::statement_iface& stmt) {
    std::size_t column = 0;
    for_each_field(proto_, [&](auto& field) {
      auto binding = make_field_binding(field);
      binding->bind(stmt, column + 1);
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
