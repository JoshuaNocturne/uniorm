#pragma once

// Predicate expression trees built from member pointers. Leaf nodes store a
// type-erased member pointer (member_key) plus bound values; column names are
// resolved against the mapping registry when SQL is generated.

#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

#include "../export.hpp"
#include "../params.hpp"
#include "../value.hpp"

namespace uniorm {

struct member_key {
  std::type_index owner;
  std::vector<std::byte> repr;

  bool operator==(member_key const&) const = default;
};

template <class T, class M>
member_key make_member_key(M T::* member) {
  member_key key{ std::type_index(typeid(T)), {} };
  key.repr.resize(sizeof(member));
  std::memcpy(key.repr.data(), &member, sizeof(member));
  return key;
}

class UNIORM_API predicate {
public:
  using resolver = std::function<std::string(member_key const&)>;

  predicate() = default;

  static predicate comparison(
    member_key key, std::string_view op, sql_value value);
  static predicate in_list(member_key key, std::vector<sql_value> values);
  static predicate null_check(member_key key, bool negated);
  static predicate like_expr(member_key key, std::string pattern);
  static predicate conjunction(predicate lhs, predicate rhs);
  static predicate disjunction(predicate lhs, predicate rhs);

  // Render to SQL with '?' placeholders, appending bound values to out.
  std::string to_sql(
    resolver const& resolve, std::vector<sql_value>& out) const;

private:
  struct node;
  std::shared_ptr<node> node_;
};

// Member-pointer comparison builders. The right-hand value is converted via
// detail::make_sql_value (enums are converted through their underlying type).
// Named functions instead of operator overloads: a templated operator== would
// collide with C++20 reversed-candidate rewriting.

template <class T, class M, class V>
predicate eq(M T::* member, V&& value) {
  return predicate::comparison(make_member_key(member), "=",
    detail::make_sql_value(std::forward<V>(value)));
}
template <class T, class M, class V>
predicate ne(M T::* member, V&& value) {
  return predicate::comparison(make_member_key(member), "<>",
    detail::make_sql_value(std::forward<V>(value)));
}
template <class T, class M, class V>
predicate lt(M T::* member, V&& value) {
  return predicate::comparison(make_member_key(member), "<",
    detail::make_sql_value(std::forward<V>(value)));
}
template <class T, class M, class V>
predicate le(M T::* member, V&& value) {
  return predicate::comparison(make_member_key(member),
    "<=", detail::make_sql_value(std::forward<V>(value)));
}
template <class T, class M, class V>
predicate gt(M T::* member, V&& value) {
  return predicate::comparison(make_member_key(member), ">",
    detail::make_sql_value(std::forward<V>(value)));
}
template <class T, class M, class V>
predicate ge(M T::* member, V&& value) {
  return predicate::comparison(make_member_key(member),
    ">=", detail::make_sql_value(std::forward<V>(value)));
}

// Column reference for fluent infix style: col(&User::age) >= 18.
template <class T, class M>
struct column_ref {
  M T::* member;
};

template <class T, class M>
column_ref<T, M> col(M T::* member) {
  return column_ref<T, M>{ member };
}

template <class T, class M, class V>
predicate operator==(column_ref<T, M> c, V&& value) {
  return eq(c.member, std::forward<V>(value));
}
template <class T, class M, class V>
predicate operator!=(column_ref<T, M> c, V&& value) {
  return ne(c.member, std::forward<V>(value));
}
template <class T, class M, class V>
predicate operator<(column_ref<T, M> c, V&& value) {
  return lt(c.member, std::forward<V>(value));
}
template <class T, class M, class V>
predicate operator<=(column_ref<T, M> c, V&& value) {
  return le(c.member, std::forward<V>(value));
}
template <class T, class M, class V>
predicate operator>(column_ref<T, M> c, V&& value) {
  return gt(c.member, std::forward<V>(value));
}
template <class T, class M, class V>
predicate operator>=(column_ref<T, M> c, V&& value) {
  return ge(c.member, std::forward<V>(value));
}

inline predicate operator&&(predicate lhs, predicate rhs) {
  return predicate::conjunction(std::move(lhs), std::move(rhs));
}
inline predicate operator||(predicate lhs, predicate rhs) {
  return predicate::disjunction(std::move(lhs), std::move(rhs));
}

template <class T, class M, class V>
predicate in(M T::* member, std::initializer_list<V> values) {
  std::vector<sql_value> converted;
  converted.reserve(values.size());
  for (auto const& v : values) {
    converted.push_back(detail::make_sql_value(v));
  }
  return predicate::in_list(make_member_key(member), std::move(converted));
}

template <class T, class M, class V>
predicate in(M T::* member, std::vector<V> const& values) {
  std::vector<sql_value> converted;
  converted.reserve(values.size());
  for (auto const& v : values) {
    converted.push_back(detail::make_sql_value(v));
  }
  return predicate::in_list(make_member_key(member), std::move(converted));
}

template <class T, class M>
predicate is_null(M T::* member) {
  return predicate::null_check(make_member_key(member), /*negated=*/false);
}

template <class T, class M>
predicate is_not_null(M T::* member) {
  return predicate::null_check(make_member_key(member), /*negated=*/true);
}

template <class T, class M>
predicate like(M T::* member, std::string_view pattern) {
  return predicate::like_expr(make_member_key(member), std::string(pattern));
}

}  // namespace uniorm
