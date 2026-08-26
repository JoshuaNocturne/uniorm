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
  virtual void materialize(std::size_t row_index) {}

  virtual void finalize(std::size_t row_index) {
    if (ind_[row_index] == backend::null_indicator) {
      throw type_mismatch("NULL value for non-optional projection field");
    }
    materialize(row_index);
  }

  std::int64_t indicator(std::size_t row_index) const noexcept {
    return ind_[row_index];
  }

  void set_row_array_size(std::size_t size) {
    row_array_size_ = size;
    ind_.resize(size, 0);
  }

protected:
  std::size_t row_array_size_ = 1;
  std::vector<std::int64_t> ind_;
};

template <class T>
class direct_binding : public field_binding {
public:
  explicit direct_binding(T& target) : target_(target) {}

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    // For block fetching, we need an array of values
    data_.resize(row_array_size_);
    // BufferLength should be the size of one element, not the total size
    // ODBC driver calculates stride automatically based on this
    stmt.bind_column(column,
      {buffer_kind(), data_.data(), sizeof(T), ind_.data()});
  }

  void materialize(std::size_t row_index) override {
    target_ = data_[row_index];
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
  std::vector<T> data_;
};

class string_binding : public field_binding {
public:
  explicit string_binding(std::string& target) : target_(target) {}

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    stmt_ = &stmt;
    column_ = column;
    // For block fetching, allocate N buffers of 4096 bytes each
    buf_.resize(row_array_size_ * 4096);
    // BufferLength should be the size of one element (4096 bytes)
    stmt.bind_column(column,
      {backend::buffer_type::chars, buf_.data(), 4096, ind_.data()});
  }

  void materialize(std::size_t row_index) override {
    std::size_t offset = row_index * 4096;
    std::int64_t ind = ind_[row_index];
    if (ind == backend::no_total || ind > 4095) {
      // Full value from the backend; replaces the partial bound buffer.
      target_ = stmt_->read_long_text(column_);
    } else {
      target_.assign(buf_.data() + offset, static_cast<std::size_t>(ind));
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
  explicit binary_binding(std::vector<std::byte>& target) : target_(target) {}

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    stmt_ = &stmt;
    column_ = column;
    // For block fetching, allocate N buffers of 4096 bytes each
    buf_.resize(row_array_size_ * 4096);
    // BufferLength should be the size of one element (4096 bytes)
    stmt.bind_column(column,
      {backend::buffer_type::bytes, buf_.data(), 4096, ind_.data()});
  }

  void materialize(std::size_t row_index) override {
    std::size_t offset = row_index * 4096;
    std::int64_t ind = ind_[row_index];
    bool truncated = ind == backend::no_total || ind > 4096;
    if (truncated) {
      target_ = stmt_->read_long_bytes(column_);  // full value, replaces
    } else {
      target_.assign(
        buf_.begin() + offset,
        buf_.begin() + offset + static_cast<std::ptrdiff_t>(ind));
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
    // For block fetching, allocate N staging areas
    staging_.resize(row_array_size_);
    // BufferLength should be the size of one element
    stmt.bind_column(column, {backend::buffer_type::timestamp_parts,
                           staging_.data(), sizeof(backend::timestamp_parts), ind_.data()});
  }

  void materialize(std::size_t row_index) override {
    auto const& s = staging_[row_index];
    target_ = make_timestamp(s.year, s.month, s.day,
      s.hour, s.minute, s.second, s.fraction_ns);
  }

private:
  timestamp& target_;
  std::vector<backend::timestamp_parts> staging_;
};

template <class T>
std::unique_ptr<field_binding> make_field_binding(T& field);

template <class T>
class optional_binding : public field_binding {
public:
  explicit optional_binding(std::optional<T>& target) : target_(target) {}

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    inner_ = make_field_binding(storage_);
    inner_->set_row_array_size(row_array_size_);
    inner_->bind(stmt, column);
  }

  void finalize(std::size_t row_index) override {
    if (inner_->indicator(row_index) == backend::null_indicator) {
      target_.reset();
    } else {
      inner_->materialize(row_index);
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
  void set_row_array_size(std::size_t size) {
    row_array_size_ = size;
  }

  void bind(backend::statement_iface& stmt) {
    std::size_t column = 0;
    for_each_field(proto_, [&](auto& field) {
      auto binding = make_field_binding(field);
      binding->set_row_array_size(row_array_size_);
      binding->bind(stmt, column + 1);
      ++column;
      bindings_.push_back(std::move(binding));
    });
  }

  // Moves the assembled row out. Call once per row in the fetched block.
  T take(std::size_t row_index) {
    for (auto& binding : bindings_) {
      binding->finalize(row_index);
    }
    return std::move(proto_);
  }

private:
  T proto_{};
  std::size_t row_array_size_ = 1;
  std::vector<std::unique_ptr<field_binding>> bindings_;
};

}  // namespace uniorm::detail
