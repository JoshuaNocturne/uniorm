#include "uniorm/dialect.hpp"

#include <algorithm>

namespace uniorm {

std::string dialect::quote_identifier(std::string_view identifier) const {
  std::string out;
  out.reserve(identifier.size() + 2);
  out.push_back(quote_open);
  out.append(identifier);
  out.push_back(quote_close);
  return out;
}

std::string dialect::pagination(
  std::optional<std::size_t> limit, std::size_t offset) const {
  if (!limit && offset == 0) {
    return {};
  }
  std::string out;
  if (ansi_pagination) {
    if (offset > 0) {
      out += " OFFSET " + std::to_string(offset) + " ROWS";
    }
    if (limit) {
      out += std::string(
               out.empty() ? " OFFSET 0 ROWS FETCH NEXT " : " FETCH NEXT ") +
             std::to_string(*limit) + " ROWS ONLY";
    }
  } else {
    if (limit) {
      out += " LIMIT " + std::to_string(*limit);
    }
    if (offset > 0) {
      out += " OFFSET " + std::to_string(offset);
    }
  }
  return out;
}

dialect dialect::detect(std::string_view dbms_name) {
  std::string lower(dbms_name);
  std::transform(lower.begin(), lower.end(), lower.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  dialect d;
  if (lower.find("mysql") != std::string::npos ||
      lower.find("mariadb") != std::string::npos) {
    d.quote_open = '`';
    d.quote_close = '`';
    d.ansi_pagination = false;
  }
  return d;
}

}  // namespace uniorm
