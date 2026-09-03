#pragma once

// Private header, not installed: resolves the WHERE field names an entity
// write was handed and derives the columns it assigns. No connection here.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <uniorm/error.hpp>
#include <uniorm/mapping/registry.hpp>

namespace uniorm::detail {

// Primary key columns in declaration order; empty when the entity maps none.
inline std::vector<std::string> primary_key_columns(entity_meta const& m) {
  std::vector<std::string> out;
  for (auto const& c : m.columns) {
    if (c.is_primary_key) {
      out.push_back(c.column);
    }
  }
  return out;
}

// Resolves WHERE field names to column indices, keeping the caller's order --
// the order the statement spells its predicates in. `op` labels diagnostics.
inline std::vector<std::size_t> resolve_where_columns(entity_meta const& m,
  std::vector<std::string> const& where_fields, std::string_view op) {
  if (where_fields.empty()) {
    throw uniorm_error(std::string(op) + ": no WHERE fields specified");
  }
  std::vector<std::size_t> out;
  out.reserve(where_fields.size());
  for (auto const& field : where_fields) {
    std::size_t found = m.columns.size();
    for (std::size_t ci = 0; ci < m.columns.size(); ++ci) {
      if (m.columns[ci].column == field) {
        found = ci;
        break;
      }
    }
    if (found == m.columns.size()) {
      throw mapping_error(
        std::string(op) + ": WHERE field '" + field + "' is not mapped");
    }
    out.push_back(found);
  }
  return out;
}

// The columns a write matches on. An absent field list means the whole primary
// key, not just its leading column -- that would match other rows too.
inline std::vector<std::size_t> resolve_where(entity_meta const& m,
  std::optional<std::vector<std::string>> const& where_fields,
  std::string_view op) {
  if (where_fields) {
    return resolve_where_columns(m, *where_fields, op);
  }
  std::vector<std::string> pk = primary_key_columns(m);
  if (pk.empty()) {
    throw uniorm_error(std::string(op) +
      ": entity has no primary key; specify where_fields explicitly");
  }
  return resolve_where_columns(m, pk, op);
}

// The predicate columns' complement, in column order.
inline std::vector<std::size_t> set_columns_of(entity_meta const& m,
  std::vector<std::size_t> const& where_col_indices) {
  std::vector<bool> in_where(m.columns.size(), false);
  for (auto ci : where_col_indices) {
    in_where[ci] = true;
  }
  std::vector<std::size_t> out;
  out.reserve(m.columns.size());
  for (std::size_t ci = 0; ci < m.columns.size(); ++ci) {
    if (!in_where[ci]) {
      out.push_back(ci);
    }
  }
  return out;
}

}  // namespace uniorm::detail
