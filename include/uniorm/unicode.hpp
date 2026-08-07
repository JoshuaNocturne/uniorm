#pragma once

#include <string>
#include <string_view>

#include "error.hpp"
#include "export.hpp"

namespace uniorm::unicode {

// UTF-8 <-> UTF-16 conversion used at the ODBC boundary (SQLWCHAR).
// Throws unicode_error on malformed input.
UNIORM_API std::u16string utf8_to_utf16(std::string_view utf8);
UNIORM_API std::string utf16_to_utf8(std::u16string_view utf16);

}  // namespace uniorm::unicode
