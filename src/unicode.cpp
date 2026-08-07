#include "uniorm/unicode.hpp"

#include <cstddef>

namespace uniorm::unicode {

namespace {

struct decoded {
  char32_t code_point;
  std::size_t length;
};

decoded decode_utf8_at(std::string_view utf8, std::size_t i) {
  auto byte = [&](std::size_t pos) {
    return static_cast<unsigned char>(utf8[pos]);
  };

  unsigned char lead = byte(i);
  std::size_t length;
  char32_t cp;

  if (lead < 0x80) {
    return { static_cast<char32_t>(lead), 1 };
  } else if ((lead & 0xE0) == 0xC0) {
    length = 2;
    cp = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    cp = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    cp = lead & 0x07;
  } else {
    throw unicode_error("invalid UTF-8 lead byte");
  }

  if (i + length > utf8.size()) {
    throw unicode_error("truncated UTF-8 sequence");
  }
  for (std::size_t k = 1; k < length; ++k) {
    unsigned char cont = byte(i + k);
    if ((cont & 0xC0) != 0x80) {
      throw unicode_error("invalid UTF-8 continuation byte");
    }
    cp = (cp << 6) | (cont & 0x3F);
  }

  // Reject overlong encodings, surrogates, and out-of-range code points.
  static constexpr char32_t min_by_length[] = { 0, 0, 0x80, 0x800, 0x10000 };
  if (cp < min_by_length[length] || cp > 0x10FFFF ||
      (cp >= 0xD800 && cp <= 0xDFFF)) {
    throw unicode_error("invalid UTF-8 code point");
  }
  return { cp, length };
}

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

std::u16string utf8_to_utf16(std::string_view utf8) {
  std::u16string out;
  out.reserve(utf8.size());
  std::size_t i = 0;
  while (i < utf8.size()) {
    auto [cp, length] = decode_utf8_at(utf8, i);
    i += length;
    if (cp <= 0xFFFF) {
      out.push_back(static_cast<char16_t>(cp));
    } else {
      char32_t v = cp - 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
    }
  }
  return out;
}

std::string utf16_to_utf8(std::u16string_view utf16) {
  std::string out;
  out.reserve(utf16.size() * 3);
  std::size_t i = 0;
  while (i < utf16.size()) {
    char32_t cp = utf16[i];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i + 1 >= utf16.size() || utf16[i + 1] < 0xDC00 ||
          utf16[i + 1] > 0xDFFF) {
        throw unicode_error("unpaired UTF-16 high surrogate");
      }
      cp = 0x10000 + ((cp - 0xD800) << 10) + (utf16[i + 1] - 0xDC00);
      i += 2;
    } else {
      if (cp >= 0xDC00 && cp <= 0xDFFF) {
        throw unicode_error("unpaired UTF-16 low surrogate");
      }
      i += 1;
    }
    append_utf8(out, cp);
  }
  return out;
}

}  // namespace uniorm::unicode
