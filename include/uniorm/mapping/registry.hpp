#pragma once

// Entity mapping registry: explicit contract between C++ types and tables.
// Column access is captured as type-erased write closures at registration
// time; runtime query materialization needs no templates.

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

#include <uniorm/detail/projection.hpp>
#include <uniorm/detail/traits.hpp>
#include <uniorm/error.hpp>
#include <uniorm/export.hpp>
#include <uniorm/params.hpp>
#include <uniorm/query/expression.hpp>
#include <uniorm/row.hpp>
#include <uniorm/value.hpp>

namespace uniorm {

class connection;

enum class validation_mode { strict, lenient };

struct column_meta {
  std::string column;
  bool is_primary_key = false;
  bool nullable = false;
  member_key key{ std::type_index(typeid(void)), {} };
  std::function<void(void*, sql_value const&)> write;
  std::function<sql_value(void const*)> read;
  // Direct ODBC binding onto the member of obj (query materialization).
  std::function<std::unique_ptr<detail::field_binding>(void*)> make_binding;
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

template <class T, class M>
column_meta make_column_meta(
  std::string_view column, M T::* member, bool primary) {
  column_meta c;
  c.column = std::string(column);
  c.is_primary_key = primary;
  c.nullable = is_optional_v<M>;
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

// Mapping registry plus schema validation entry point. Not thread-safe; hold
// one per thread/session.
class UNIORM_API orm {
public:
  orm() = default;

  template <class T>
  mapping_builder<T> map(std::string_view table) {
    std::type_index type(typeid(T));
    if (entities_.count(type) != 0) {
      throw mapping_error(
        std::string("entity already registered: ") + typeid(T).name());
    }
    entity_meta& meta = entities_[type];
    meta.table = table;
    return mapping_builder<T>(meta);
  }

  template <class T>
  entity_meta const& meta() const {
    auto it = entities_.find(std::type_index(typeid(T)));
    if (it == entities_.end()) {
      throw mapping_error(
        std::string("entity not registered: ") + typeid(T).name());
    }
    return it->second;
  }

  entity_meta const* find(std::type_index type) const {
    auto it = entities_.find(type);
    return it == entities_.end() ? nullptr : &it->second;
  }

  std::size_t size() const noexcept {
    return entities_.size();
  }

  // Reconcile all registered mappings against the live schema via ODBC
  // metadata. Throws mapping_error on the first mismatch.
  void validate(
    connection& conn, validation_mode mode = validation_mode::strict);

private:
  std::unordered_map<std::type_index, entity_meta> entities_;
};

}  // namespace uniorm
