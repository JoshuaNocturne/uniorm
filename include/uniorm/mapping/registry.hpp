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
#include <uniorm/detail/projection.hpp>
#include <uniorm/detail/time.hpp>
#include <uniorm/detail/traits.hpp>
#include <uniorm/error.hpp>
#include <uniorm/export.hpp>
#include <uniorm/builder/expression.hpp>
#include <uniorm/row.hpp>
#include <uniorm/value.hpp>

namespace uniorm {

enum class validation_mode { strict, lenient };

struct column_meta {
  std::string column;
  bool is_primary_key = false;
  bool nullable = false;
  backend::buffer_type buffer_type = backend::buffer_type::chars;
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

}  // namespace detail

// Member types the value layer can read directly (or wrapped in optional).
template <class M>
concept readable_member =
  detail::plain_sql_member<std::remove_cvref_t<M>> ||
  (detail::is_optional_v<M> &&
    detail::plain_sql_member<typename std::remove_cvref_t<M>::value_type>);

namespace detail {

// Helper to get buffer_type for a member type
template <class M>
constexpr backend::buffer_type member_buffer_type() {
  using U = std::remove_cvref_t<M>;
  if constexpr (is_optional_v<M>) {
    return member_buffer_type<typename M::value_type>();
  } else if constexpr (std::is_same_v<U, bool>) {
    return backend::buffer_type::bit;
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
    return backend::buffer_type::chars;
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
    return make_field_binding(static_cast<T*>(obj)->*member);
  };
  c.write_to_param_buffer = [member](void const* obj, std::size_t row,
                                      void* buffer, std::size_t stride,
                                      std::int64_t* indicators)
    -> backend::buffer_type {
    using U = std::remove_cvref_t<M>;
    auto const& field = static_cast<T const*>(obj)->*member;

    if constexpr (is_optional_v<M>) {
      if (!field.has_value()) {
        indicators[row] = backend::null_indicator;
        return member_buffer_type<M>();
      }
      auto const& value = *field;
      if constexpr (std::is_same_v<typename M::value_type, std::string>) {
        std::memcpy(static_cast<char*>(buffer) + row * stride, value.data(),
          value.size());
        indicators[row] = static_cast<std::int64_t>(value.size());
      } else if constexpr (std::is_same_v<typename M::value_type,
                           std::vector<std::byte>>) {
        std::memcpy(static_cast<char*>(buffer) + row * stride, value.data(),
          value.size());
        indicators[row] = static_cast<std::int64_t>(value.size());
      } else if constexpr (std::is_same_v<typename M::value_type, timestamp>) {
        auto src = detail::break_timestamp(value);
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
        indicators[row] = sizeof(value);
        using value_type = typename M::value_type;
        static_cast<value_type*>(buffer)[row] = value;
      }
      return member_buffer_type<M>();
    } else {
      if constexpr (std::is_same_v<U, std::string>) {
        std::memcpy(static_cast<char*>(buffer) + row * stride, field.data(),
          field.size());
        indicators[row] = static_cast<std::int64_t>(field.size());
      } else if constexpr (std::is_same_v<U, std::vector<std::byte>>) {
        std::memcpy(static_cast<char*>(buffer) + row * stride, field.data(),
          field.size());
        indicators[row] = static_cast<std::int64_t>(field.size());
      } else if constexpr (std::is_same_v<U, timestamp>) {
        auto src = detail::break_timestamp(field);
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
        indicators[row] = sizeof(field);
        static_cast<U*>(buffer)[row] = field;
      }
      return member_buffer_type<M>();
    }
  };
  c.get_string_size = [member](void const* obj) -> std::size_t {
    using U = std::remove_cvref_t<M>;
    auto const& field = static_cast<T const*>(obj)->*member;

    if constexpr (is_optional_v<M>) {
      if (!field.has_value()) {
        return 0;
      }
      auto const& value = *field;
      if constexpr (std::is_same_v<typename M::value_type, std::string>) {
        return value.size();
      } else if constexpr (std::is_same_v<typename M::value_type,
                           std::vector<std::byte>>) {
        return value.size();
      } else {
        return 0;
      }
    } else {
      if constexpr (std::is_same_v<U, std::string>) {
        return field.size();
      } else if constexpr (std::is_same_v<U, std::vector<std::byte>>) {
        return field.size();
      } else {
        return 0;
      }
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
      "sql type or std::optional thereof");
    meta_.columns.push_back(detail::make_column_meta(column, member, false));
    return *this;
  }

  template <class M>
  mapping_builder& primary_key(std::string_view column, M T::* member) {
    static_assert(readable_member<M>,
      "member type not supported by the value layer; use a supported "
      "sql type or std::optional thereof");
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
