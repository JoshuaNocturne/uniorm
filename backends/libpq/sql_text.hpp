#pragma once

// Private header: the rewrite that turns the contract's placeholders into the
// ones libpq binds by. Not installed.

#include <cstddef>
#include <string>
#include <string_view>

#include "export.hpp"

namespace uniorm::libpq {

// Every `?` becomes `$n`, numbered in the order the marks appear. A `?` that
// sits inside a literal, a delimited name, a dollar-quoted body or a comment
// is that construct's own character, so it is copied instead of numbered, and
// so is the second of a doubled `??` -- this server reads a lone `?` as an
// operator of its own, which a marker may not eat. The count it found comes
// back through placeholder_count, which is also how many parameters the
// statement declares.
UNIORM_LIBPQ_API std::string rewrite_placeholders(
  std::string_view sql, std::size_t& placeholder_count);

}  // namespace uniorm::libpq
