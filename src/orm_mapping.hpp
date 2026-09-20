#pragma once

// Private header, not installed: resolves the WHERE field names an entity
// write was handed, derives the columns it assigns, and checks a mapping
// against a catalog. No connection object here.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <uniorm/detail/identifier.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/error.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/schema.hpp>

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

// --- Validation ---

// The one name in `have` differing from `wanted` by case alone: two spellings
// of the same object, which is what a mapping carries between servers.
inline std::optional<std::string> case_variant(
  std::vector<std::string> const& have, std::string_view wanted) {
  std::string const folded = fold_lower(wanted);
  for (auto const& name : have) {
    if (name != wanted && fold_lower(name) == folded) {
      return name;
    }
  }
  return std::nullopt;
}

inline std::string case_hint(std::optional<std::string> const& alt) {
  return alt ? " (only case differs from '" + *alt + "')" : std::string();
}

// One entity against the catalog's own spellings, which a miss has to name:
// rejecting the pair without them leaves the caller comparing by eye. Each
// name is asked for as the dialect will emit it, so a miss is reported in the
// one spelling the policy can be fixed in.
inline void validate_entity(schema_meta& md, entity_meta const& m,
  validation_mode mode, dialect const& d) {
  std::string const table = d.fold_identifier(m.table);
  auto live = md.shape({ {}, {}, table });
  if (live.empty()) {
    // The whole catalog is searched, so a candidate is shown where it lives:
    // the same name under another schema is not the table this mapping wants.
    std::string const folded = fold_lower(table);
    std::string candidate;
    for (auto const& row : md.tables({}, {})) {
      if (row.name != table && fold_lower(row.name) == folded) {
        candidate =
          row.schema.empty() ? row.name : row.schema + "." + row.name;
        break;
      }
    }
    throw mapping_error("table not found: " + table +
      (candidate.empty() ? std::string()
                         : " (only case differs from '" + candidate + "')"));
  }
  std::vector<std::string> columns;
  columns.reserve(live.size());
  for (auto const& c : live) {
    columns.push_back(c.name);
  }
  for (auto const& c : m.columns) {
    std::string const column = d.fold_identifier(c.column);
    auto* found = find_column(live, column);
    if (found == nullptr) {
      throw mapping_error("column not found in table " + table + ": " +
        column + case_hint(case_variant(columns, column)));
    }
    if (mode == validation_mode::lenient) {
      continue;
    }
    // sql_type::other is a type no backend could be blamed for misreading:
    // with no family to compare, there is nothing to check.
    if (found->type != sql_type::other &&
        (c.accepted_types & sql_type_bit(found->type)) == 0) {
      throw mapping_error("column " + table + "." + column + " is " +
        sql_type_name(found->type) +
        ", which the mapped member does not bind");
    }
    if (found->nullable && !c.nullable) {
      throw mapping_error("column " + table + "." + column +
        " is nullable but the mapped member is not std::optional");
    }
  }
}

}  // namespace uniorm::detail
