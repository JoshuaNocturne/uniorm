#pragma once

#include <string>
#include <string_view>

namespace uniorm::gen {

// t_user -> TUser; splits on '_'/'-' and on case boundaries. The result is
// a valid C++ identifier: a leading digit gains a '_' prefix and a keyword
// collision gains a '_' suffix. Never returns an empty string.
std::string to_pascal_case(std::string_view identifier);

// user_id -> userId; same rules as to_pascal_case.
std::string to_camel_case(std::string_view identifier);

// Lowercase [a-z0-9_] identifier suitable for namespace/header names; a
// leading digit gains a '_' prefix. Never returns an empty string.
std::string to_unit_name(std::string_view identifier);

}  // namespace uniorm::gen
