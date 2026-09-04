#pragma once

// Entity mapping registry: column_meta, entity_meta, mapping_builder.

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/converter.hpp>
#include <uniorm/detail/projection.hpp>
#include <uniorm/detail/time.hpp>
#include <uniorm/detail/traits.hpp>
#include <uniorm/error.hpp>
#include <uniorm/export.hpp>
#include <uniorm/builder/expression.hpp>
#include <uniorm/row.hpp>
#include <uniorm/types.hpp>
#include <uniorm/value.hpp>

namespace uniorm {

enum class validation_mode { strict, lenient };

struct column_meta {
  std::string column;
  bool is_primary_key = false;
  bool nullable = false;
  backend::buffer_type buffer_type = backend::buffer_type::chars;
  // SQL types the member's representation binds; strict validation compares
  // the live column type against it.
  sql_type_set accepted_types = 0;
  member_key key{ std::type_index(typeid(void)), {} };
  std::function<void(void*, sql_value const&)> write;
  std::function<sql_value(void const*)> read;
  // Direct backend binding onto the member of obj (query materialization).
  std::function<std::unique_ptr<detail::field_binding>(void*)> make_binding;
  // Direct write to parameter buffer for batch insert (bypasses sql_value).
  // Returns buffer type, writes value from obj to buffer[row*stride], sets indicator.
  std::function<backend::buffer_type(void const* obj, std::size_t row,
    void* buffer, std::size_t stride, std::int64_t* indicators)>
    write_to_param_buffer;
  // Get string/binary size without creating sql_value (for buffer pre-allocation).
  // Returns 0 for non-string/binary types, or the size for string/binary types.
  std::function<std::size_t(void const* obj)> get_string_size;
};

struct UNIORM_API entity_meta {
  std::string table;
  std::vector<column_meta> columns;
  std::vector<member_key> ignored;

  std::string const& column_name(
    member_key const& key) const;  // throws mapping_error
  void populate(void* obj, row const& r) const;
};

namespace detail {

template <class U>
concept plain_sql_member =
  std::is_same_v<U, bool> || std::is_same_v<U, std::int8_t> ||
  std::is_same_v<U, std::int16_t> || std::is_same_v<U, std::int32_t> ||
  std::is_same_v<U, std::int64_t> || std::is_same_v<U, double> ||
  std::is_same_v<U, std::string> || std::is_same_v<U, std::vector<std::byte>> ||
  std::is_same_v<U, timestamp>;

// A domain type reaches a column through its converter's representation, so
// everything below that keys on the member type asks the converter first.
template <class U>
concept sql_representable = plain_sql_member<U> || has_converter<U>;

}  // namespace detail

// Member types the value layer can read, wrapped in optional or not.
template <class M>
concept readable_member =
  detail::sql_representable<std::remove_cvref_t<M>> ||
  (detail::is_optional_v<M> &&
    detail::sql_representable<typename std::remove_cvref_t<M>::value_type>);

namespace detail {

// Bytes a representation occupies in a chars/bytes parameter buffer.
template <class V>
std::size_t param_size(V const& value) {
  if constexpr (std::is_same_v<V, std::string> ||
                std::is_same_v<V, std::vector<std::byte>>) {
    return value.size();
  } else {
    return 0;
  }
}

// Stage one row's representation into its slot of a batch parameter buffer.
template <class V>
void stage_param(V const& value, std::size_t row, void* buffer,
  std::size_t stride, std::int64_t* indicators) {
  using U = std::remove_cvref_t<V>;
  if constexpr (std::is_same_v<U, std::string> ||
                std::is_same_v<U, std::vector<std::byte>>) {
    std::memcpy(static_cast<char*>(buffer) + row * stride, value.data(),
      value.size());
    indicators[row] = static_cast<std::int64_t>(value.size());
  } else if constexpr (std::is_same_v<U, timestamp>) {
    auto src = break_timestamp(value);
    auto* dst = reinterpret_cast<backend::timestamp_parts*>(
      static_cast<char*>(buffer) + row * stride);
    dst->year = static_cast<std::int16_t>(src.year);
    dst->month = static_cast<std::uint16_t>(src.month);
    dst->day = static_cast<std::uint16_t>(src.day);
    dst->hour = static_cast<std::uint16_t>(src.hour);
    dst->minute = static_cast<std::uint16_t>(src.minute);
    dst->second = static_cast<std::uint16_t>(src.second);
    dst->fraction_ns = static_cast<std::uint32_t>(src.fraction_ns);
    indicators[row] = sizeof(backend::timestamp_parts);
  } else {
    indicators[row] = static_cast<std::int64_t>(sizeof(U));
    static_cast<U*>(buffer)[row] = value;
  }
}

// A converter-backed value is encoded before it is staged, so the buffer copy
// that follows is the one its representation already makes.
template <class V>
void write_param(V const& value, std::size_t row, void* buffer,
  std::size_t stride, std::int64_t* indicators) {
  if constexpr (has_converter<std::remove_cvref_t<V>>) {
    using U = std::remove_cvref_t<V>;
    converter_sql<U> encoded{};
    converter<U>::to_db(value, encoded);
    stage_param(encoded, row, buffer, stride, indicators);
  } else {
    stage_param(value, row, buffer, stride, indicators);
  }
}

// The prescan cannot measure a converter column without encoding it: the
// width of the representation is the column's row stride.
template <class V>
std::size_t encoded_param_size(V const& value) {
  if constexpr (has_converter<std::remove_cvref_t<V>>) {
    using U = std::remove_cvref_t<V>;
    converter_sql<U> encoded{};
    converter<U>::to_db(value, encoded);
    return param_size(encoded);
  } else {
    return param_size(value);
  }
}

// Helper to get buffer_type for a member type
template <class M>
constexpr backend::buffer_type member_buffer_type() {
  using U = std::remove_cvref_t<M>;
  if constexpr (is_optional_v<M>) {
    return member_buffer_type<typename M::value_type>();
  } else if constexpr (has_converter<U>) {
    return member_buffer_type<converter_sql<U>>();
  } else if constexpr (std::is_same_v<U, bool>) {
    return backend::buffer_type::bit;
  } else if constexpr (std::is_same_v<U, std::int8_t>) {
    return backend::buffer_type::int8;
  } else if constexpr (std::is_same_v<U, std::int16_t>) {
    return backend::buffer_type::int16;
  } else if constexpr (std::is_same_v<U, std::int32_t>) {
    return backend::buffer_type::int32;
  } else if constexpr (std::is_same_v<U, std::int64_t>) {
    return backend::buffer_type::int64;
  } else if constexpr (std::is_same_v<U, double>) {
    return backend::buffer_type::float64;
  } else if constexpr (std::is_same_v<U, std::string>) {
    return backend::buffer_type::chars;
  } else if constexpr (std::is_same_v<U, std::vector<std::byte>>) {
    return backend::buffer_type::bytes;
  } else if constexpr (std::is_same_v<U, timestamp>) {
    return backend::buffer_type::timestamp_parts;
  } else {
    static_assert(
      std::is_same_v<U, U> && false,
      "a converter's sql type must be one uniorm binds directly");
    return backend::buffer_type::chars;
  }
}

// The SQL types a representation binds, as the type table promises. Anything
// outside the set reaches the member only by driver-side coercion, so strict
// validation reports it rather than trusting the driver to get it right.
template <class M>
constexpr sql_type_set accepted_sql_types() {
  using U = std::remove_cvref_t<M>;
  if constexpr (is_optional_v<M>) {
    return accepted_sql_types<typename M::value_type>();
  } else if constexpr (has_converter<U>) {
    return accepted_sql_types<converter_sql<U>>();
  } else if constexpr (std::is_same_v<U, bool>) {
    return sql_type_bit(sql_type::boolean);
  } else if constexpr (std::is_same_v<U, std::int8_t> ||
                       std::is_same_v<U, std::int16_t>) {
    // TINYINT and SMALLINT both normalize to smallint.
    return sql_type_bit(sql_type::smallint);
  } else if constexpr (std::is_same_v<U, std::int32_t>) {
    return sql_type_bit(sql_type::integer);
  } else if constexpr (std::is_same_v<U, std::int64_t>) {
    return sql_type_bit(sql_type::bigint);
  } else if constexpr (std::is_same_v<U, double>) {
    return sql_type_bit(sql_type::real) |
      sql_type_bit(sql_type::double_precision) |
      sql_type_bit(sql_type::decimal);
  } else if constexpr (std::is_same_v<U, std::string>) {
    return sql_type_bit(sql_type::character) | sql_type_bit(sql_type::varchar) |
      sql_type_bit(sql_type::longvarchar) | sql_type_bit(sql_type::wchar) |
      sql_type_bit(sql_type::wvarchar) | sql_type_bit(sql_type::guid) |
      sql_type_bit(sql_type::decimal);
  } else if constexpr (std::is_same_v<U, std::vector<std::byte>>) {
    return sql_type_bit(sql_type::binary) | sql_type_bit(sql_type::varbinary);
  } else if constexpr (std::is_same_v<U, timestamp>) {
    return sql_type_bit(sql_type::date) | sql_type_bit(sql_type::time) |
      sql_type_bit(sql_type::timestamp);
  } else {
    return 0;
  }
}

template <class T, class M>
column_meta make_column_meta(
  std::string_view column, M T::* member, bool primary) {
  column_meta c;
  c.column = std::string(column);
  c.is_primary_key = primary;
  c.nullable = is_optional_v<M>;
  c.buffer_type = member_buffer_type<M>();
  c.accepted_types = accepted_sql_types<M>();
  c.key = make_member_key(member);
  c.write = [member](void* obj, sql_value const& v) {
    static_cast<T*>(obj)->*member = value_cast<M>(v);
  };
  c.read = [member](void const* obj) -> sql_value {
    auto const& v = static_cast<T const*>(obj)->*member;
    if constexpr (is_optional_v<M>) {
      if (!v.has_value()) {
        return std::monostate{};
      }
      return make_sql_value(*v);
    } else {
      return make_sql_value(v);
    }
  };
  c.make_binding = [member](void* obj) {
    return make_field_binding(static_cast<T*>(obj)->*member, obj);
  };
  c.write_to_param_buffer = [member](void const* obj, std::size_t row,
                                      void* buffer, std::size_t stride,
                                      std::int64_t* indicators)
    -> backend::buffer_type {
    auto const& field = static_cast<T const*>(obj)->*member;
    if constexpr (is_optional_v<M>) {
      if (!field.has_value()) {
        indicators[row] = backend::null_indicator;
        return member_buffer_type<M>();
      }
      write_param(*field, row, buffer, stride, indicators);
    } else {
      write_param(field, row, buffer, stride, indicators);
    }
    return member_buffer_type<M>();
  };
  c.get_string_size = [member](void const* obj) -> std::size_t {
    auto const& field = static_cast<T const*>(obj)->*member;
    if constexpr (is_optional_v<M>) {
      return field.has_value() ? encoded_param_size(*field) : 0;
    } else {
      return encoded_param_size(field);
    }
  };
  return c;
}

}  // namespace detail

class orm;

template <class T>
class mapping_builder {
public:
  explicit mapping_builder(entity_meta& meta) : meta_(meta) {}

  template <class M>
  mapping_builder& column(std::string_view column, M T::* member) {
    static_assert(readable_member<M>,
      "member type not supported by the value layer; use a supported "
      "sql type, std::optional thereof, or specialize uniorm::converter");
    meta_.columns.push_back(detail::make_column_meta(column, member, false));
    return *this;
  }

  template <class M>
  mapping_builder& primary_key(std::string_view column, M T::* member) {
    static_assert(readable_member<M>,
      "member type not supported by the value layer; use a supported "
      "sql type, std::optional thereof, or specialize uniorm::converter");
    meta_.columns.push_back(detail::make_column_meta(column, member, true));
    return *this;
  }

  template <class M>
  mapping_builder& ignore(M T::* member) {
    meta_.ignored.push_back(make_member_key(member));
    return *this;
  }

private:
  entity_meta& meta_;
};

}  // namespace uniorm
