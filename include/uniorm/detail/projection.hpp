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
#include <uniorm/converter.hpp>
#include <uniorm/error.hpp>
#include <uniorm/value.hpp>
#include <uniorm/detail/pfr.hpp>
#include <uniorm/detail/time.hpp>
#include <uniorm/detail/traits.hpp>

namespace uniorm::detail {

// Byte offset of `field` within the entity object based at `base`.
template <class F>
std::ptrdiff_t member_offset(F const& field, void* base) noexcept {
  return static_cast<char const*>(static_cast<void const*>(&field)) -
    static_cast<char*>(base);
}

// Binds one result column and writes fetched rows directly into an entity
// of the mapped layout. The binding captures the field's byte offset within
// the entity; finalize() materializes a fetched row into any entity
// instance (the result vector's storage), so no intermediate prototype or
// per-row move of small (SSO) strings is required.
class field_binding {
public:
  virtual ~field_binding() = default;

  virtual void bind(backend::statement_iface& stmt, std::size_t column) = 0;
  // entity points at a fully constructed object of the binding's layout.
  virtual void finalize(std::size_t row_index, void* entity) {
    if (ind_[row_index] == backend::null_indicator) reject_null();
    write(row_index, entity);
  }

  // Row-level NULL flag of this binding's own bound array.
  virtual std::int64_t indicator(std::size_t row_index) const noexcept {
    return ind_[row_index];
  }

  void set_row_array_size(std::size_t size) {
    row_array_size_ = size;
    ind_.resize(size, 0);
  }

protected:
  static void reject_null() {
    throw type_mismatch("NULL value for non-optional projection field");
  }

  // Copy staging into the entity's field; assumes data is non-NULL.
  virtual void write(std::size_t row_index, void* entity) = 0;

  std::size_t row_array_size_ = 1;
  std::ptrdiff_t offset_ = 0;
  std::vector<std::int64_t> ind_;
};

template <class T>
class direct_binding : public field_binding {
public:
  direct_binding(T& target, void* base)
    : field_binding() {
    offset_ = member_offset(target, base);
  }

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    // For block fetching, we need an array of values
    data_.resize(row_array_size_);
    // BufferLength should be the size of one element, not the total size
    // ODBC driver calculates stride automatically based on this
    stmt.bind_column(column,
      {buffer_kind(), data_.data(), sizeof(T), ind_.data()});
  }

  void write(std::size_t row_index, void* entity) override {
    *reinterpret_cast<T*>(static_cast<char*>(entity) + offset_) =
      data_[row_index];
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

  std::vector<T> data_;
};

class string_binding : public field_binding {
public:
  string_binding(std::string& target, void* base) {
    offset_ = member_offset(target, base);
  }

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    stmt_ = &stmt;
    column_ = column;
    // Size each staging slot after the column's declared width instead of
    // a fixed maximum: tight strides keep a fetched block cache-resident.
    // display_size is in characters; *4 bounds the UTF-8 byte length.
    // Oversized values fall back to read_long_text per row.
    std::size_t slot = 4096;
    auto meta = stmt.column_meta();
    if (column - 1 < meta.size()) {
      std::size_t chars = meta[column - 1].display_size;
      if (chars > 0 && chars < 1024) slot = chars * 4 + 1;
    }
    slot_size_ = slot;
    buf_.resize(row_array_size_ * slot_size_);
    stmt.bind_column(column,
      {backend::buffer_type::chars, buf_.data(), slot_size_, ind_.data()});
  }

  void write(std::size_t row_index, void* entity) override {
    std::string& target =
      *reinterpret_cast<std::string*>(static_cast<char*>(entity) + offset_);
    std::size_t offset = row_index * slot_size_;
    std::int64_t ind = ind_[row_index];
    if (ind == backend::no_total ||
        ind > static_cast<std::int64_t>(slot_size_) - 1) {
      // Full value from the backend; replaces the partial bound buffer.
      target = stmt_->read_long_text(column_);
    } else {
      auto len = static_cast<std::size_t>(ind);
      target.reserve(len);
      target.assign(buf_.data() + offset, len);
    }
  }

private:
  std::vector<char> buf_;
  std::size_t slot_size_ = 4096;
  backend::statement_iface* stmt_ = nullptr;
  std::size_t column_ = 0;
};

class binary_binding : public field_binding {
public:
  binary_binding(std::vector<std::byte>& target, void* base) {
    offset_ = member_offset(target, base);
  }

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    stmt_ = &stmt;
    column_ = column;
    // Tight stride per declared column width; see string_binding::bind.
    // display_size is in bytes for binary columns.
    std::size_t slot = 4096;
    auto meta = stmt.column_meta();
    if (column - 1 < meta.size()) {
      std::size_t bytes = meta[column - 1].display_size;
      if (bytes > 0 && bytes < 4096) slot = bytes + 1;
    }
    slot_size_ = slot;
    buf_.resize(row_array_size_ * slot_size_);
    stmt.bind_column(column,
      {backend::buffer_type::bytes, buf_.data(), slot_size_, ind_.data()});
  }

  void write(std::size_t row_index, void* entity) override {
    std::vector<std::byte>& target = *reinterpret_cast<std::vector<std::byte>*>(
      static_cast<char*>(entity) + offset_);
    std::size_t offset = row_index * slot_size_;
    std::int64_t ind = ind_[row_index];
    bool truncated = ind == backend::no_total ||
      ind > static_cast<std::int64_t>(slot_size_);
    if (truncated) {
      target = stmt_->read_long_bytes(column_);  // full value, replaces
    } else {
      auto len = static_cast<std::size_t>(ind);
      target.reserve(len);
      target.assign(
        buf_.begin() + offset,
        buf_.begin() + offset + static_cast<std::ptrdiff_t>(len));
    }
  }

private:
  std::vector<std::byte> buf_;
  std::size_t slot_size_ = 4096;
  backend::statement_iface* stmt_ = nullptr;
  std::size_t column_ = 0;
};

class timestamp_binding : public field_binding {
public:
  timestamp_binding(timestamp& target, void* base) {
    offset_ = member_offset(target, base);
  }

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    // For block fetching, allocate N staging areas
    staging_.resize(row_array_size_);
    // BufferLength should be the size of one element
    stmt.bind_column(column, {backend::buffer_type::timestamp_parts,
                           staging_.data(), sizeof(backend::timestamp_parts), ind_.data()});
  }

  void write(std::size_t row_index, void* entity) override {
    auto const& s = staging_[row_index];
    *reinterpret_cast<timestamp*>(static_cast<char*>(entity) + offset_) =
      make_timestamp(s.year, s.month, s.day, s.hour, s.minute, s.second,
        s.fraction_ns);
  }

private:
  std::vector<backend::timestamp_parts> staging_;
};

template <class T>
std::unique_ptr<field_binding> make_field_binding(T& field, void* base);

template <class T>
class optional_binding : public field_binding {
public:
  optional_binding(std::optional<T>& target, void* base) {
    offset_ = member_offset(target, base);
  }

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    inner_ = make_field_binding(storage_, &storage_);
    inner_->set_row_array_size(row_array_size_);
    inner_->bind(stmt, column);
  }

  void finalize(std::size_t row_index, void* entity) override {
    auto& target = *reinterpret_cast<std::optional<T>*>(
      static_cast<char*>(entity) + offset_);
    if (inner_->indicator(row_index) == backend::null_indicator) {
      target.reset();
    } else {
      inner_->finalize(row_index, &storage_);
      target = std::move(storage_);
    }
  }

  // finalize() is fully overridden; the NULL-checking base path never
  // reaches write() for optionals.
  void write(std::size_t, void*) override {}

  // The inner binding owns the array the backend writes.
  std::int64_t indicator(std::size_t row_index) const noexcept override {
    return inner_->indicator(row_index);
  }

private:
  T storage_{};
  std::unique_ptr<field_binding> inner_;
};

// Binds a converter domain type as its db_type and maps each fetched row
// through from_db. Staging belongs to the inner binding, so the row lands
// through the buffers a field of that db_type would have bound; the slot moved
// out of is dead to this binding either way, so a decode that keeps its buffer
// costs no more than that field's own value would have.
template <class T>
class converter_binding : public field_binding {
public:
  converter_binding(T& target, void* base) {
    offset_ = member_offset(target, base);
  }

  void bind(backend::statement_iface& stmt, std::size_t column) override {
    inner_ = make_field_binding(slot_, &slot_);
    inner_->set_row_array_size(row_array_size_);
    inner_->bind(stmt, column);
  }

  // This binding binds nothing, so every NULL test has to read the inner
  // array.
  std::int64_t indicator(std::size_t row_index) const noexcept override {
    return inner_->indicator(row_index);
  }

  void finalize(std::size_t row_index, void* entity) override {
    if (indicator(row_index) == backend::null_indicator) reject_null();
    inner_->finalize(row_index, &slot_);
    *reinterpret_cast<T*>(static_cast<char*>(entity) + offset_) =
      converter<T>::from_db(std::move(slot_));
  }

  // finalize() is fully overridden; the base path never reaches write().
  void write(std::size_t, void*) override {}

private:
  converter_db_type<T> slot_{};
  std::unique_ptr<field_binding> inner_;
};

template <class T>
concept directly_bindable =
  std::is_same_v<T, bool> || std::is_same_v<T, std::int8_t> ||
  std::is_same_v<T, std::int16_t> || std::is_same_v<T, std::int32_t> ||
  std::is_same_v<T, std::int64_t> || std::is_same_v<T, float> ||
  std::is_same_v<T, double>;

template <class T>
std::unique_ptr<field_binding> make_field_binding(T& field, void* base) {
  using U = std::remove_cvref_t<T>;
  if constexpr (is_optional_v<U>) {
    return std::make_unique<optional_binding<typename U::value_type>>(
      field, base);
  } else if constexpr (std::is_same_v<U, std::string>) {
    return std::make_unique<string_binding>(field, base);
  } else if constexpr (std::is_same_v<U, std::vector<std::byte>>) {
    return std::make_unique<binary_binding>(field, base);
  } else if constexpr (std::is_same_v<U, timestamp>) {
    return std::make_unique<timestamp_binding>(field, base);
  } else if constexpr (directly_bindable<U>) {
    return std::make_unique<direct_binding<U>>(field, base);
  } else if constexpr (has_converter<U>) {
    return std::make_unique<converter_binding<U>>(field, base);
  } else {
    static_assert(std::is_same_v<U, U> && false,
      "unsupported projection field type: use a supported sql type, "
      "std::optional thereof, or specialize uniorm::converter for it");
  }
}

template <class T>
class projection {
public:
  void set_row_array_size(std::size_t size) {
    row_array_size_ = size;
  }

  void bind(backend::statement_iface& stmt) {
    // Offsets are captured against a throwaway instance; they are identical
    // for every object of the aggregate layout.
    T proto{};
    std::size_t column = 0;
    for_each_field(proto, [&](auto& field) {
      auto binding = make_field_binding(field, &proto);
      binding->set_row_array_size(row_array_size_);
      binding->bind(stmt, column + 1);
      ++column;
      bindings_.push_back(std::move(binding));
    });
  }

  // Materialize one row directly into `dest` (vector storage); bindings
  // write at captured member offsets, so no prototype round trip is needed.
  void fill_into(T& dest, std::size_t row_index) {
    for (auto& binding : bindings_) {
      binding->finalize(row_index, &dest);
    }
  }

private:
  std::size_t row_array_size_ = 1;
  std::vector<std::unique_ptr<field_binding>> bindings_;
};

}  // namespace uniorm::detail
