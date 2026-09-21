#include "uniorm/dialect.hpp"

#include <uniorm/detail/identifier.hpp>
#include <uniorm/error.hpp>

namespace uniorm {

std::string dialect::fold_identifier(std::string_view identifier) const {
  switch (identifiers) {
  case identifier_case::lower:
    return detail::fold_lower(identifier);
  case identifier_case::upper:
    return detail::fold_upper(identifier);
  case identifier_case::keep:
    break;
  }
  return std::string(identifier);
}

std::string dialect::quote_identifier(std::string_view identifier) const {
  return quote_exact_identifier(fold_identifier(identifier));
}

std::string dialect::quote_exact_identifier(
  std::string_view identifier) const {
  if (identifier.find('\0') != std::string_view::npos) {
    throw uniorm_error("identifier contains a NUL byte");
  }
  std::string quoted;
  quoted.reserve(identifier.size() + 2);
  quoted.push_back(quote_open);
  for (char character : identifier) {
    quoted.push_back(character);
    if (character == quote_close) {
      quoted.push_back(quote_close);
    }
  }
  quoted.push_back(quote_close);
  return quoted;
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
  std::string lower = detail::fold_lower(dbms_name);
  dialect detected;
  if (lower.find("mysql") != std::string::npos ||
      lower.find("mariadb") != std::string::npos) {
    detected.quote_open = '`';
    detected.quote_close = '`';
    detected.ansi_pagination = false;
    detected.table_qualification = qualification::catalog;
  } else if (lower.find("postgresql") != std::string::npos) {
    detected.table_qualification = qualification::schema;
  }
  return detected;
}

}  // namespace uniorm
