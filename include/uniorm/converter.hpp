#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

namespace uniorm {

// Extension point that lets a domain type stand as an entity field or a query
// parameter. A type gets exactly one database representation: two columns that
// store the same domain type differently is a schema difference, and the remedy
// is a distinct domain type, not a second specialization.
template <class T>
struct converter {};  // specialize: db_type, to_db, from_db

// to_db assigns into a caller-owned slot rather than returning one: a
// representation longer than the short-string buffer would otherwise allocate
// once per row on every batch write. It must overwrite the slot, not extend it.
// from_db is called with an rvalue, because the caller is done with the slot
// and a converter that stores the representation can steal it rather than
// copy; one that names db_type const& satisfies this too and pays the copy.
template <class T>
concept has_converter = requires(T const& value,
                                 typename converter<T>::db_type& slot) {
  typename converter<T>::db_type;
  converter<T>::to_db(value, slot);
  { converter<T>::from_db(std::move(slot)) } -> std::same_as<T>;
};

namespace detail {

template <class T>
using converter_db_type = typename converter<T>::db_type;

}  // namespace detail

}  // namespace uniorm
