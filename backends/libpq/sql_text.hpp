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

// The row group of a statement that inserts exactly one row, cut out of the
// text `rewrite_placeholders` produced. A batch of these is the same statement
// with the group repeated, so the fold is worth having; anything else -- an
// UPDATE, a DELETE, an insert that binds outside its group, a group whose
// entries are not plain parameters -- comes back with no columns, which is how
// "make no fold" reads.
struct UNIORM_LIBPQ_API values_fold {
  std::size_t columns = 0;
  std::string head;  // up to the group's '('
  std::string tail;  // from its ')' on
};

// `placeholder_count` is what the rewrite reported: the group has to carry
// every parameter the statement declares, and nothing else may.
UNIORM_LIBPQ_API values_fold find_values_fold(
  std::string_view rewritten, std::size_t placeholder_count);

// The fold for `rows` rows: the head, then that many groups whose parameters
// are numbered on from one another, then the tail.
UNIORM_LIBPQ_API std::string expand_values_fold(
  values_fold const& fold, std::size_t rows);

}  // namespace uniorm::libpq
