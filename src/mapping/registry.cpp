#include "uniorm/mapping/registry.hpp"

#include <uniorm/backend/error.hpp>
#include "../identifier.hpp"

namespace uniorm {

namespace {

// `identifier_resolution` is a public field, so a caller that hand-builds it
// can hand back fewer (or more) names than the mapping has columns. Every
// resolved access path indexes the two vectors side by side; check once at
// each entry so the failure names the mismatch instead of throwing
// std::out_of_range from a `.at()` in a hot render.
void require_resolution_size(entity_meta const& meta) {
  if (meta.resolved->columns.size() != meta.columns.size()) {
    throw mapping_error(
      "identifier resolution does not match the mapped column count");
  }
}

}  // namespace

std::string const& entity_meta::column_name(member_key const& key) const {
  for (auto const& c : columns) {
    if (c.key == key) {
      return c.column;
    }
  }
  throw mapping_error("member is not mapped to a column");
}

std::string entity_meta::table_sql(dialect const& sql_dialect) const {
  if (!resolved) {
    return sql_dialect.quote_identifier(table);
  }
  std::string_view qualifier;
  switch (sql_dialect.table_qualification) {
  case dialect::qualification::schema:
    qualifier = resolved->table.schema;
    break;
  case dialect::qualification::catalog:
    qualifier = resolved->table.catalog;
    break;
  case dialect::qualification::unsupported:
    throw backend::capability_not_supported(
      "this dialect cannot render resolved identifiers");
  }
  std::string sql;
  if (!qualifier.empty()) {
    sql = sql_dialect.quote_exact_identifier(qualifier) + ".";
  }
  return sql + sql_dialect.quote_exact_identifier(resolved->table.name);
}

std::string entity_meta::column_sql(
  std::size_t index, dialect const& sql_dialect) const {
  auto const& column = columns.at(index);
  if (!resolved) {
    return sql_dialect.quote_identifier(column.column);
  }
  require_resolution_size(*this);
  if (sql_dialect.table_qualification == dialect::qualification::unsupported) {
    throw backend::capability_not_supported(
      "this dialect cannot render resolved identifiers");
  }
  return sql_dialect.quote_exact_identifier(resolved->columns.at(index));
}

std::string entity_meta::column_sql(
  member_key const& key, dialect const& sql_dialect) const {
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (columns[index].key == key) {
      return column_sql(index, sql_dialect);
    }
  }
  throw mapping_error("member is not mapped to a column");
}

void entity_meta::populate(void* obj, row const& result_row) const {
  if (resolved) {
    require_resolution_size(*this);
  }
  for (std::size_t index = 0; index < columns.size(); ++index) {
    auto const& column = columns[index];
    auto const& label = resolved ? resolved->columns.at(index) : column.column;
    column.write(obj, result_row.at(label));
  }
}

std::size_t entity_meta::result_ordinal(
  std::size_t index, std::vector<column_info> const& result) const {
  if (resolved) {
    require_resolution_size(*this);
  }
  auto const& label = resolved
    ? resolved->columns.at(index) : columns[index].column;
  for (std::size_t slot = 0; slot < result.size(); ++slot) {
    if (result[slot].name == label) {
      return slot + 1;
    }
  }
  // A quoted declared name can select a column the server reports with other
  // casing, so an exact miss still matches a folded label.
  for (std::size_t slot = 0; slot < result.size(); ++slot) {
    if (detail::fold_equal(result[slot].name, label)) {
      return slot + 1;
    }
  }
  throw column_not_found("result set has no column \"" + label + "\"");
}

}  // namespace uniorm
