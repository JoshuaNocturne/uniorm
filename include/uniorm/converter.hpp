#pragma once

#include <concepts>

namespace uniorm {

template <class Cpp, class Sql>
struct converter;  // users specialize: static to_db(Cpp const&) / from_db(Sql
                   // const&)

template <class Cpp, class Sql>
concept has_converter = requires(Cpp const& c, Sql const& s) {
  converter<Cpp, Sql>::to_db(c);
  converter<Cpp, Sql>::from_db(s);
};

}  // namespace uniorm
