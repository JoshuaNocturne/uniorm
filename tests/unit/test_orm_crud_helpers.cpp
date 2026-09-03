// Mapping-level normalization behind orm's entity CRUD: primary key inference,
// WHERE resolution, the set/predicate split and the batch/fetch size clamps.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "check.hpp"

#include "orm_mapping.hpp"

#include <uniorm/error.hpp>
#include <uniorm/orm.hpp>

using namespace uniorm;

namespace {

struct Entry {
  std::int64_t bucket = 0;
  std::int64_t ordinal = 0;
  std::string label;
  std::int32_t weight = 0;
};

struct Solo {
  std::int64_t id = 0;
  std::string label;
};

struct Keyless {
  std::string label;
  std::int32_t weight = 0;
};

// Junction table: every column is a key column, so nothing is left to assign.
struct Link {
  std::int64_t left = 0;
  std::int64_t right = 0;
};

orm build_registries() {
  orm registry;
  registry.map<Entry>("entries")
    .primary_key("bucket", &Entry::bucket)
    .primary_key("ordinal", &Entry::ordinal)
    .column("label", &Entry::label)
    .column("weight", &Entry::weight);
  registry.map<Solo>("solo")
    .primary_key("id", &Solo::id)
    .column("label", &Solo::label);
  registry.map<Keyless>("keyless")
    .column("label", &Keyless::label)
    .column("weight", &Keyless::weight);
  registry.map<Link>("links")
    .primary_key("left", &Link::left)
    .primary_key("right", &Link::right);
  return registry;
}

}  // namespace

void test_orm_crud_helpers() {
  orm registry = build_registries();
  entity_meta const& m = registry.meta<Entry>();

  // Matching only the leading key column would address unrelated rows.
  auto const composite = std::vector<std::string>{ "bucket", "ordinal" };
  CHECK(detail::primary_key_columns(m) == composite);
  auto const single = std::vector<std::string>{ "id" };
  CHECK(detail::primary_key_columns(registry.meta<Solo>()) == single);
  CHECK(detail::primary_key_columns(registry.meta<Keyless>()).empty());

  // Resolution keeps the caller's order, which is the order the statement
  // spells its predicates in.
  auto const fields = std::vector<std::string>{ "label", "bucket" };
  auto const indices = detail::resolve_where_columns(m, fields, "update");
  auto const expected_indices = std::vector<std::size_t>{ 2, 0 };
  CHECK(indices == expected_indices);

  auto const repeated = std::vector<std::string>{ "bucket", "bucket" };
  auto const repeated_indices =
    detail::resolve_where_columns(m, repeated, "update");
  auto const expected_repeated = std::vector<std::size_t>{ 0, 0 };
  CHECK(repeated_indices == expected_repeated);

  // An unmapped name throws here, not later as a driver placeholder mismatch.
  auto const unknown = std::vector<std::string>{ "label", "colour" };
  CHECK_THROWS(
    detail::resolve_where_columns(m, unknown, "update"), mapping_error);
  CHECK_THROWS(
    detail::resolve_where_columns(m, unknown, "remove"), mapping_error);
  auto const none = std::vector<std::string>{};
  CHECK_THROWS(detail::resolve_where_columns(m, none, "update"), uniorm_error);

  // Without an explicit field list the whole key is the predicate; a keyless
  // entity has nothing to match on and must not degrade into a bulk write.
  auto const inferred = detail::resolve_where(m, std::nullopt, "update");
  auto const expected_inferred = std::vector<std::size_t>{ 0, 1 };
  CHECK(inferred == expected_inferred);
  auto const solo_key = std::vector<std::size_t>{ 0 };
  CHECK(detail::resolve_where(registry.meta<Solo>(), std::nullopt, "remove") ==
    solo_key);
  CHECK_THROWS(
    detail::resolve_where(registry.meta<Keyless>(), std::nullopt, "update"),
    uniorm_error);
  // An explicit list still wins over the key.
  CHECK(detail::resolve_where(m, fields, "update") == indices);

  // The assignment side is the predicate side's complement, in column order.
  auto const set_indices = detail::set_columns_of(m, indices);
  auto const expected_set = std::vector<std::size_t>{ 1, 3 };
  CHECK(set_indices == expected_set);
  // One entry per predicate and one per assignment, covering every column.
  CHECK(set_indices.size() + indices.size() == m.columns.size());

  auto const everything = detail::set_columns_of(m, {});
  CHECK(everything.size() == m.columns.size());

  // A junction entity leaves nothing to assign; the write reports that.
  CHECK(detail::set_columns_of(registry.meta<Link>(), { 0, 1 }).empty());

  // The chunk loop advances by this, so zero is clamped at the setter.
  orm db;
  CHECK(db.paramset_size() == orm::default_paramset_size);
  db.paramset_size(0);
  CHECK(db.paramset_size() == orm::default_paramset_size);
  db.paramset_size(7);
  CHECK(db.paramset_size() == 7);

  // Likewise the block fetch size: buffers are divided by it to find a slot.
  CHECK(db.row_array_size() == orm::default_row_array_size);
  db.row_array_size(0);
  CHECK(db.row_array_size() == orm::default_row_array_size);
  db.row_array_size(25);
  CHECK(db.row_array_size() == 25);
}
