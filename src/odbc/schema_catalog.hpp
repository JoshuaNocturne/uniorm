#pragma once

// Private header: the ODBC implementation of uniorm::schema_meta,
// built on the driver's catalog procedures. Not installed.

#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <uniorm/detail/identifier.hpp>
#include <uniorm/schema.hpp>
#include "connection.hpp"

namespace uniorm::odbc {

// The catalog calls take their names as pattern values, so a driver may
// answer with objects nobody asked about. A row whose reported name differs
// from the one asked for is such a row; case alone is not a difference, and
// a name the driver left out or an argument left wide open keeps the row.
inline bool catalog_name_matches(
  std::string_view asked, std::string_view reported) {
  return asked.empty() || reported.empty() ||
    uniorm::detail::fold_lower(asked) == uniorm::detail::fold_lower(reported);
}

inline SQLSMALLINT exact_catalog_arg_length(std::string_view text) {
  if (text.find('\0') != std::string_view::npos ||
      text.size() > static_cast<std::size_t>(
        std::numeric_limits<SQLSMALLINT>::max())) {
    throw backend::capability_not_supported(
      "exact catalog argument contains NUL or exceeds the ODBC length limit");
  }
  return static_cast<SQLSMALLINT>(text.size());
}

inline char catalog_pattern_escape(std::string_view escape) {
  if (escape.size() != 1 || escape.front() == '\0') {
    throw backend::capability_not_supported(
      "exact catalog reads require a single-byte search pattern escape");
  }
  return escape.front();
}

inline std::string catalog_pattern_literal(
  std::string_view text, std::string_view escape) {
  exact_catalog_arg_length(text);
  char escape_character = catalog_pattern_escape(escape);
  std::string pattern;
  for (char character : text) {
    if (character == '%' || character == '_' || character == escape_character) {
      pattern += escape_character;
    }
    pattern += character;
  }
  exact_catalog_arg_length(pattern);
  return pattern;
}

inline std::string exact_tables_catalog_arg(
  std::string_view catalog, std::string_view escape) {
  exact_catalog_arg_length(catalog);
  // SQLTables catalog patterns are optional; raw wildcards include themselves.
  if (catalog.find(catalog_pattern_escape(escape)) != std::string_view::npos) {
    throw backend::capability_not_supported(
      "SQLTables cannot guarantee catalogs containing the pattern escape");
  }
  return std::string(catalog);
}

inline std::string exact_catalog_text(std::string_view buffer,
  SQLLEN indicator, std::string_view field, bool allow_empty = false) {
  if (indicator == SQL_NULL_DATA && allow_empty) {
    return {};
  }
  if (indicator < 0 ||
      static_cast<std::size_t>(indicator) >= buffer.size()) {
    throw odbc_error(
      "exact catalog metadata has missing or truncated " + std::string(field),
      {});
  }
  auto length = static_cast<std::size_t>(indicator);
  auto text = buffer.substr(0, length);
  if ((!allow_empty && text.empty()) ||
      text.find('\0') != std::string_view::npos || buffer[length] != '\0') {
    throw odbc_error(
      "exact catalog metadata has invalid " + std::string(field), {});
  }
  return std::string(text);
}

inline bool exact_catalog_namespace_matches(std::string_view catalog,
  std::string_view schema, schema_meta::table_row const& reported) {
  return catalog == reported.catalog && schema == reported.schema;
}

inline bool exact_catalog_table_matches(schema_meta::table_ref const& asked,
  schema_meta::table_row const& reported) {
  return exact_catalog_namespace_matches(asked.catalog, asked.schema,
           reported) && asked.name == reported.name;
}

struct odbc_schema_meta : schema_meta {
  explicit odbc_schema_meta(connection& conn) : conn_(conn) {}

  std::string database_name() override;
  std::vector<table_row> tables(
    std::string_view catalog, std::string_view schema) override;
  std::vector<column_row> table_columns(table_ref const&) override;
  std::vector<table_row> exact_tables(
    std::string_view catalog, std::string_view schema) override;
  std::vector<column_row> exact_table_columns(table_ref const&) override;
  std::vector<std::string> primary_key(table_ref const&) override;
  std::vector<foreign_key_row> foreign_keys(table_ref const&) override;
  std::vector<index_row> indexes(table_ref const&) override;

  connection& conn_;
};

}  // namespace uniorm::odbc
