#include "schema_reader.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "naming.hpp"
#include "uniorm/error.hpp"

namespace uniorm::gen {

namespace {

using meta = schema_meta;

// The identity the backend listed the table under, which is what the
// per-table reads have to be narrowed by.
meta::table_ref ref_of(table_model const& table) {
  return meta::table_ref{ table.catalog, table.schema, table.name };
}

std::vector<table_model> list_tables(
  meta& md, read_options const& opts) {
  std::vector<table_model> all;
  for (auto const& row : md.tables(opts.catalog, opts.schema)) {
    table_model m;
    m.catalog = row.catalog;
    m.schema = row.schema;
    m.name = row.name;
    all.push_back(std::move(m));
  }

  if (opts.tables.empty()) {
    return all;
  }
  std::vector<table_model> selected;
  for (std::string const& want : opts.tables) {
    table_model const* hit = nullptr;
    for (table_model const& have : all) {
      if (have.name == want) {
        hit = &have;
        break;
      }
    }
    if (hit == nullptr) {
      // Some servers (lower_case_table_names) report table names with
      // different case than requested.
      std::string const folded = fold_lower(want);
      for (table_model const& have : all) {
        if (fold_lower(have.name) == folded) {
          hit = &have;
          break;
        }
      }
    }
    if (hit == nullptr) {
      throw uniorm_error("table not found: " + want);
    }
    selected.push_back(*hit);
  }
  return selected;
}

void read_columns(meta& md, table_model& table) {
  for (auto const& row : md.table_columns(ref_of(table))) {
    column_model col;
    col.shape = row.shape;
    col.type_name = row.type_name;
    col.size = row.size;
    col.decimals = row.decimals;
    col.default_value = row.default_value;
    table.columns.push_back(std::move(col));
  }
}

// Reports the key names that matched no column: a second catalog read spelling
// the same column differently would otherwise drop the flag in silence.
std::vector<std::string> read_primary_keys(meta& md, table_model& table) {
  std::vector<std::string> unmatched;
  for (std::string const& pk : md.primary_key(ref_of(table))) {
    auto hit = std::find_if(table.columns.begin(), table.columns.end(),
      [&](column_model const& c) { return c.shape.name == pk; });
    if (hit != table.columns.end()) {
      hit->primary_key = true;
      continue;
    }
    std::string folded = fold_lower(pk);
    auto alt = std::find_if(table.columns.begin(), table.columns.end(),
      [&](column_model const& c) {
        return fold_lower(c.shape.name) == folded;
      });
    std::string name = pk;
    if (alt != table.columns.end()) {
      name += " (only case differs from '" + alt->shape.name + "')";
    }
    unmatched.push_back(std::move(name));
  }
  return unmatched;
}

void read_foreign_keys(meta& md, table_model& table) {
  // One row per column pair, in key order within each referenced table, so
  // grouping by that table in encounter order rebuilds the constraints.
  for (auto const& row : md.foreign_keys(ref_of(table))) {
    auto it = std::find_if(table.foreign_keys.begin(),
      table.foreign_keys.end(),
      [&](fk_model const& m) { return m.pk_table == row.referenced_table; });
    if (it == table.foreign_keys.end()) {
      table.foreign_keys.push_back(fk_model{ row.referenced_table, {} });
      it = std::prev(table.foreign_keys.end());
    }
    it->columns.emplace_back(row.column, row.referenced_column);
  }
}

void read_indexes(meta& md, table_model& table) {
  auto rows = md.indexes(ref_of(table));
  // Names order the generated comments, whatever order the catalog
  // happened to walk the indexes in.
  std::sort(rows.begin(), rows.end(),
    [](meta::index_row const& a, meta::index_row const& b) {
      return a.name < b.name;
    });
  for (auto const& row : rows) {
    index_model idx;
    idx.name = row.name;
    idx.columns = row.columns;
    idx.unique = row.unique;
    table.indexes.push_back(std::move(idx));
  }
}

// The catalog reads ask by name, so a wildcard in one of these arguments
// stands for itself and narrows the list to nothing. Saying so beats an empty
// run.
void warn_pattern(std::string_view flag, std::string_view value,
  std::vector<std::string>* warnings) {
  if (warnings != nullptr && value.find('%') != std::string_view::npos) {
    warnings->push_back(std::string(flag) + "='" + std::string(value) +
                        "': catalog names are asked as names, '%' is not a "
                        "wildcard here");
  }
}

}  // namespace

schema_model read_schema(schema_meta& md,
  read_options const& opts, std::vector<std::string>* warnings) {
  schema_model model;
  warn_pattern("--catalog", opts.catalog, warnings);
  warn_pattern("--schema", opts.schema, warnings);
  auto tables = list_tables(md, opts);
  for (table_model& table : tables) {
    read_columns(md, table);
    std::vector<std::string> unmatched = read_primary_keys(md, table);
    read_foreign_keys(md, table);
    read_indexes(md, table);
    if (warnings != nullptr) {
      if (table.columns.empty()) {
        warnings->push_back("table has no columns: " + table.name);
      }
      for (std::string const& pk : unmatched) {
        warnings->push_back(
          table.name + ": primary key names no such column: " + pk);
      }
    }
    model.tables.push_back(std::move(table));
  }
  return model;
}

}  // namespace uniorm::gen
