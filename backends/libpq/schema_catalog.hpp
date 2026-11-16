#pragma once

// Private header: the libpq implementation of uniorm::schema_meta, built on
// pg_catalog reads. Not installed.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "connection.hpp"
#include "native_types.hpp"
#include <uniorm/schema.hpp>

namespace uniorm::libpq {

// The size and scale a column's type modifier declares, in the terms the
// catalog states them: an exact number carries precision above scale, a
// blessed variable-length type carries a width, anything else declares nothing.
struct column_metrics {
  std::int32_t size = 0;
  std::int16_t decimals = 0;
};

inline column_metrics catalog_column_metrics(unsigned native, int modifier) {
  if (native == pg_type::numeric) {
    return { declared_precision(modifier),
      static_cast<std::int16_t>(declared_scale(modifier)) };
  }
  if (native == pg_type::varchar || native == pg_type::bpchar ||
      native == pg_type::bytea) {
    return { static_cast<std::int32_t>(declared_length(modifier)), 0 };
  }
  return {};
}

// pg_catalog stores identifiers as the exact bytes the server spells them, so
// these reads answer names verbatim -- no pattern escaping or case re-filter
// the ODBC catalog procedures need. Each read joins the session's open
// transaction so it sees the same uncommitted work the caller is inside.
struct pq_schema_meta : schema_meta {
  explicit pq_schema_meta(connection& conn) : conn_(conn) {}

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

}  // namespace uniorm::libpq