#pragma once

// Live-schema introspection: the reads a connection's database can answer
// about the tables it sees. `orm::schema()` and `connection::schema()` hand
// out an implementation; names come back exactly as the server spells them.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uniorm/backend/error.hpp>
#include <uniorm/types.hpp>

namespace uniorm {

// What a column is, in the three terms a mapping is checked against. The
// catalog states them as facts; a mapped column answers the same questions
// with a name, the set of types its member binds, and whether the member is
// optional. So the comparison between the two is directional, and this is
// the side carrying the fact.
struct column_shape {
  std::string name;
  sql_type type = sql_type::other;
  bool nullable = false;
};

using table_shape = std::vector<column_shape>;

// Catalog order, so a diagnostic names the column the way the server does.
// Linear on purpose: the callers validate and generate, they do not serve
// rows.
inline column_shape const* find_column(
  table_shape const& shape, std::string_view column) {
  for (auto const& c : shape) {
    if (c.name == column) {
      return &c;
    }
  }
  return nullptr;
}

struct schema_meta {
  // Every name here is asked for as a name, not a pattern, and a read answers
  // with the rows the server reports under that name. An empty catalog or
  // schema leaves that part to whatever the connection already sees.
  struct table_ref {
    std::string catalog;
    std::string schema;
    std::string name;
  };

  struct table_row {
    std::string catalog;
    std::string schema;
    std::string name;
  };

  // A column as the catalog states it: the shape, plus what only a live
  // read carries.
  struct column_row {
    column_shape shape;
    std::string type_name;  // the server's own spelling of the type
    std::int32_t size = 0;
    std::int16_t decimals = 0;
    std::optional<std::string> default_value;
  };

  // One column pair of a foreign key: `column` here references
  // `referenced_column` there. A constraint over n columns yields n rows.
  struct foreign_key_row {
    std::string referenced_table;
    std::string referenced_column;
    std::string column;
  };

  struct index_row {
    std::string name;
    std::vector<std::string> columns;  // key order
    bool unique = false;
  };

  virtual ~schema_meta() = default;

  // The database the connection sits in, empty when there is none.
  virtual std::string database_name() = 0;
  // Tables an entity can map onto: not views, not system tables. Which
  // catalog word carries that is the backend's business.
  virtual std::vector<table_row> tables(
    std::string_view catalog, std::string_view schema) = 0;
  virtual std::vector<column_row> table_columns(table_ref const&) = 0;
  virtual std::vector<table_row> exact_tables(
    std::string_view, std::string_view) {
    throw backend::capability_not_supported(
      "exact namespace table enumeration is not supported");
  }
  virtual std::vector<column_row> exact_table_columns(table_ref const&) {
    throw backend::capability_not_supported(
      "exact table column metadata is not supported");
  }
  virtual std::vector<std::string> primary_key(table_ref const&) = 0;
  virtual std::vector<foreign_key_row> foreign_keys(table_ref const&) = 0;
  virtual std::vector<index_row> indexes(table_ref const&) = 0;

  // The shapes of a table's columns, in one read. An empty answer is how
  // callers see a missing table.
  table_shape shape(table_ref const& ref) {
    table_shape out;
    for (auto& row : table_columns(ref)) {
      out.push_back(std::move(row.shape));
    }
    return out;
  }
};

}  // namespace uniorm
