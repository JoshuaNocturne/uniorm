#pragma once

// Private header, not installed: resolves the WHERE field names an entity
// write was handed, derives the columns it assigns, and checks a mapping
// against a catalog. No connection object here.

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

inline std::string qualified_name(schema_meta::table_ref const& table) {
  std::string name = table.catalog;
  if (!table.schema.empty()) {
    if (!name.empty()) {
      name += ".";
    }
    name += table.schema;
  }
  if (!table.name.empty()) {
    if (!name.empty()) {
      name += ".";
    }
    name += table.name;
  }
  return name;
}

inline std::string resolve_identifier(std::vector<std::string> const& names,
  std::string_view declared, std::string_view kind, std::string_view owner) {
  std::string const folded = fold_lower(declared);
  std::vector<std::string> candidates;
  for (auto const& name : names) {
    if (name == declared) {
      return name;
    }
    if (fold_lower(name) == folded) {
      candidates.push_back(name);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
    candidates.end());
  if (candidates.size() == 1) {
    return candidates.front();
  }
  std::string const context = std::string(kind) + " in " + std::string(owner);
  if (candidates.empty()) {
    throw mapping_error(std::string(kind) + " not found in " +
      std::string(owner) + ": " + std::string(declared));
  }
  std::string message = "ambiguous " + context + ": " +
    std::string(declared) + " (candidates: ";
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (index != 0) {
      message += ", ";
    }
    message += candidates[index];
  }
  throw mapping_error(message + ")");
}

inline std::vector<schema_meta::column_row> exact_columns(
  schema_meta& metadata, schema_meta::table_ref const& table) {
  auto columns = metadata.exact_table_columns(table);
  std::sort(columns.begin(), columns.end(),
    [](auto const& left, auto const& right) {
      return left.shape.name < right.shape.name;
    });
  for (std::size_t index = 0; index < columns.size(); ++index) {
    auto const& column = columns[index];
    if (column.shape.name.empty()) {
      throw mapping_error("unnamed catalog column in " + qualified_name(table));
    }
    if (index == 0 || columns[index - 1].shape.name != column.shape.name) {
      continue;
    }
    auto const& previous = columns[index - 1];
    if (previous.shape.type != column.shape.type ||
        previous.shape.nullable != column.shape.nullable ||
        previous.type_name != column.type_name ||
        previous.size != column.size ||
        previous.decimals != column.decimals ||
        previous.default_value != column.default_value) {
      throw mapping_error("conflicting catalog column: " +
        qualified_name(table) + "." + column.shape.name);
    }
  }
  columns.erase(std::unique(columns.begin(), columns.end(),
    [](auto const& left, auto const& right) {
      return left.shape.name == right.shape.name;
    }), columns.end());
  return columns;
}

class identifier_catalog {
public:
  identifier_catalog(schema_meta& metadata, std::string_view catalog_name,
    std::string_view schema_name)
    : metadata_(metadata), scope_{ std::string(catalog_name),
        std::string(schema_name), {} } {
    for (auto const& table : metadata.exact_tables(catalog_name, schema_name)) {
      if (table.catalog == catalog_name && table.schema == schema_name) {
        if (table.name.empty()) {
          throw mapping_error("unnamed catalog table in " +
            qualified_name(scope_));
        }
        tables_.push_back(table.name);
      }
    }
    std::sort(tables_.begin(), tables_.end());
    tables_.erase(std::unique(tables_.begin(), tables_.end()), tables_.end());
  }

  identifier_resolution resolve(entity_meta const& entity) {
    identifier_resolution result;
    result.table = scope_;
    result.table.name = resolve_identifier(
      tables_, entity.table, "table", qualified_name(scope_));
    if (entity.columns.empty()) {
      return result;
    }
    auto [entry, inserted] = columns_.try_emplace(result.table.name);
    if (inserted) {
      entry->second = exact_columns(metadata_, result.table);
    }
    std::vector<std::string> names;
    names.reserve(entry->second.size());
    for (auto const& column : entry->second) {
      names.push_back(column.shape.name);
    }
    std::set<std::string> assigned;
    result.columns.reserve(entity.columns.size());
    for (auto const& column : entity.columns) {
      auto name = resolve_identifier(
        names, column.column, "column", qualified_name(result.table));
      if (!assigned.insert(name).second) {
        throw mapping_error("multiple mapped columns resolve to " +
          qualified_name(result.table) + "." + name);
      }
      result.columns.push_back(std::move(name));
    }
    return result;
  }

private:
  schema_meta& metadata_;
  schema_meta::table_ref scope_;
  std::vector<std::string> tables_;
  std::map<std::string, std::vector<schema_meta::column_row>> columns_;
};

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

// The catalog reads a validation needs. The table list is only asked for once a
// name has missed: a mapping that passes never pays for the scan.
class catalog {
public:
  // What the listing says about one spelling: whether it is carried as it was
  // asked for, and whether the same letters live under another case.
  struct spellings {
    bool exact = false;
    std::optional<std::string> variant;
  };

  explicit catalog(schema_meta& md) : md_(md) {}

  table_shape shape(std::string_view table) {
    return md_.shape({ {}, {}, std::string(table) });
  }

  table_shape exact_shape(schema_meta::table_ref const& table) {
    table_shape result;
    for (auto& column : exact_columns(md_, table)) {
      result.push_back(std::move(column.shape));
    }
    return result;
  }

  // One scan answers both: the whole catalog is searched, so a variant says
  // where it lives -- the same name under another schema is not the table this
  // mapping wants.
  spellings spellings_of(std::string_view table) {
    std::string const folded = fold_lower(table);
    spellings out;
    for (auto const& row : md_.tables({}, {})) {
      if (row.name == table) {
        out.exact = true;
      } else if (!out.variant && fold_lower(row.name) == folded) {
        out.variant =
          row.schema.empty() ? row.name : row.schema + "." + row.name;
      }
    }
    return out;
  }

private:
  schema_meta& md_;
};

// The rejection one table name answers for the whole mapping, named in the
// spelling this catalog has when it has the letters at all.
inline std::string table_not_found(catalog::spellings const& spell,
  std::string const& table) {
  return "table not found: " + table + case_hint(spell.variant);
}

inline void validate_entity(catalog& catalog, entity_meta const& entity,
  validation_mode mode, dialect const& dialect) {
  auto const& resolved = entity.resolved;
  std::string const table = resolved
    ? qualified_name(resolved->table) : dialect.fold_identifier(entity.table);
  auto live = resolved
    ? catalog.exact_shape(resolved->table) : catalog.shape(table);
  if (live.empty()) {
    if (resolved) {
      throw mapping_error("table not found: " + table);
    }
    throw mapping_error(table_not_found(catalog.spellings_of(table), table));
  }
  std::vector<std::string> columns;
  columns.reserve(live.size());
  for (auto const& column : live) {
    columns.push_back(column.name);
  }
  for (std::size_t index = 0; index < entity.columns.size(); ++index) {
    auto const& mapped = entity.columns[index];
    std::string const column = resolved
      ? resolved->columns[index] : dialect.fold_identifier(mapped.column);
    auto* found = find_column(live, column);
    if (found == nullptr) {
      if (!resolved) {
        // Listings may contain both cases, or omit views entirely.
        auto const spell = catalog.spellings_of(table);
        if (!spell.exact && spell.variant) {
          throw mapping_error(table_not_found(spell, table));
        }
      }
      throw mapping_error("column not found in table " + table + ": " +
        column + case_hint(case_variant(columns, column)));
    }
    if (mode == validation_mode::lenient) {
      continue;
    }
    if (found->type != sql_type::other &&
        (mapped.accepted_types & sql_type_bit(found->type)) == 0) {
      throw mapping_error("column " + table + "." + column + " is " +
        sql_type_name(found->type) +
        ", which the mapped member does not bind");
    }
    if (found->nullable && !mapped.nullable) {
      throw mapping_error("column " + table + "." + column +
        " is nullable but the mapped member is not std::optional");
    }
  }
}

}  // namespace uniorm::detail
