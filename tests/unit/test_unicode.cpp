#include <string>

#include <uniorm/unicode.hpp>

#include "check.hpp"

using namespace uniorm;

void test_unicode() {
  // "abc 中文 😀" — ASCII, BMP CJK, and a supplementary-plane emoji.
  std::string src = "abc \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80";

  std::u16string u16 = unicode::utf8_to_utf16(src);
  CHECK(u16.size() == 9);  // 4 + 2 + 1 + 2 (surrogate pair)
  CHECK(u16[4] == u'\u4E2D');
  CHECK(u16[7] == char16_t{ 0xD83D });
  CHECK(u16[8] == char16_t{ 0xDE00 });

  CHECK(unicode::utf16_to_utf8(u16) == src);

  CHECK(unicode::utf8_to_utf16("").empty());
  CHECK(unicode::utf16_to_utf8(u"").empty());

  // Malformed input must throw.
  CHECK_THROWS(
    unicode::utf8_to_utf16(std::string_view("\xC3\x28", 2)), unicode_error);
  CHECK_THROWS(
    unicode::utf8_to_utf16(std::string_view("\xE4\xB8", 2)), unicode_error);
  CHECK_THROWS(
    unicode::utf8_to_utf16(std::string_view("\xED\xA0\x80", 3)), unicode_error);
  CHECK_THROWS(
    unicode::utf16_to_utf8(std::u16string_view(u"\xD800", 1)), unicode_error);
  CHECK_THROWS(
    unicode::utf16_to_utf8(std::u16string_view(u"a\xDC00", 2)), unicode_error);
}
