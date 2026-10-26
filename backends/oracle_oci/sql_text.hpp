#pragma once

// Private header: the placeholder rewrite this backend owes the contract in
// <uniorm/backend/backend.hpp>, which hands prepared SQL with `?` for every
// parameter. Not installed.

#include <cstddef>
#include <string>
#include <string_view>

#include "export.hpp"

namespace uniorm::oracle_oci {

// Rewrites the contract's `?` placeholders into OCI's numbered bind variables
// (`:1`, `:2`, ...) in the order they appear, so a bind index counted from the
// caller's parameter list lands on the same placeholder. Returns the number
// of placeholders rewritten through `count`.
//
// A `?` inside a character literal, an alternate-quoted literal, a delimited
// identifier or a comment is text, not a placeholder, and stays put. Text
// ending inside a quote or a block comment is broken SQL; saying so here beats
// binding nothing and letting the server guess.
UNIORM_ORACLE_OCI_API std::string rewrite_placeholders(
  std::string_view sql, std::size_t& count);

// Whether the statement's first word, past blanks and comments, is one that
// answers with a result set. The OCI statement owes its select-list describe
// before it can bind columns, and asks for it only of a query: a describe pass
// over an INSERT or a DDL statement would stall the write it was meant to do.
UNIORM_ORACLE_OCI_API bool leads_with_query(std::string_view sql);

}  // namespace uniorm::oracle_oci
