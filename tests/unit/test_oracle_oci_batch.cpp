#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <uniorm/params.hpp>
#include <uniorm/value.hpp>

#include "oracle_oci/statement.hpp"

#include "check.hpp"

using namespace uniorm;

namespace {

using oracle_oci::batch_kind;
using oracle_oci::plan_batch;

// One row of a batch, from its values written out.
params row_of(std::vector<sql_value> cells) {
  return params(std::move(cells));
}

}  // namespace

// What each column of a batch would be staged as, read off the rows alone.
void test_oracle_oci_batch_shapes() {
  // Numbers of every width share one OCINumber per row, and a column that
  // mixes them is still one column. Text costs the widest row's bytes.
  {
    auto plan = plan_batch({ row_of({ std::int64_t{ 1 }, std::string("a") }),
      row_of({ true, std::string("bcd") }),
      row_of({ 2.5, std::monostate{} }) });
    CHECK(plan.has_value());
    CHECK(plan->size() == 2);
    CHECK(plan->at(0).kind == batch_kind::number);
    CHECK(plan->at(0).stride == sizeof(OCINumber));
    CHECK(plan->at(1).kind == batch_kind::text);
    CHECK(plan->at(1).stride == 3);
  }

  // A timestamp binds as one descriptor pointer per row, bytes as a stride of
  // them beside their own lengths, and neither cares what the row beside it
  // leaves NULL.
  {
    auto plan = plan_batch({ row_of({ timestamp{},
        std::vector<std::byte>{ std::byte{ 1 } } }),
      row_of({ std::monostate{},
        std::vector<std::byte>(7, std::byte{ 2 }) }) });
    CHECK(plan.has_value());
    CHECK(plan->at(0).kind == batch_kind::timestamp);
    CHECK(plan->at(0).stride == sizeof(OCIDateTime*));
    CHECK(plan->at(1).kind == batch_kind::binary);
    CHECK(plan->at(1).stride == 7);
  }

  // A column every row leaves NULL has no shape to read off a value, and still
  // needs a byte per row to be bound as.
  {
    auto plan = plan_batch({ row_of({ std::monostate{}, std::int32_t{ 4 } }),
      row_of({ std::monostate{}, std::monostate{} }) });
    CHECK(plan.has_value());
    CHECK(plan->at(0).kind == batch_kind::empty);
    CHECK(plan->at(0).stride == 1);
    CHECK(plan->at(1).kind == batch_kind::number);
  }
}

// The batches that are not one execute's work, so the row by row route takes
// them instead.
void test_oracle_oci_batch_refusals() {
  // Nothing to run: an empty batch has no shape to read.
  CHECK(!plan_batch({}).has_value());
  // Nothing to bind: a statement with no placeholders is not array work.
  CHECK(!plan_batch({ row_of({}), row_of({}) }).has_value());
  // Rows of different widths leave a column of the shorter row unread.
  CHECK(!plan_batch({ row_of({ std::int32_t{ 1 }, std::int32_t{ 2 } }),
    row_of({ std::int32_t{ 3 } }) }).has_value());
  // A column holding two shapes cannot be one slab: a row's text has no home
  // beside another row's number.
  CHECK(!plan_batch({ row_of({ std::int32_t{ 1 } }),
    row_of({ std::string("one") }) }).has_value());
  // Bytes and text are two shapes too, whatever one server makes of them.
  CHECK(!plan_batch({ row_of({ std::vector<std::byte>{ std::byte{ 1 } } }),
    row_of({ std::string("a") }) }).has_value());
  // A row's length reaches OCI in two bytes, and a longer one would be told
  // back to it short.
  CHECK(!plan_batch({ row_of({ std::string(65536, 'x') }) }).has_value());

  // The budget of slabs is the batch's rather than one column's: two columns
  // that each cost under the whole budget cost more than it together, and the
  // only rule that can see that is the one over the batch.
  std::string const wide(60000, 'x');
  std::vector<params> over;
  std::vector<params> within;
  for (std::size_t row = 0; row < 300; ++row) {
    over.push_back(row_of({ wide, wide }));
    if (row < 100) {
      within.push_back(row_of({ wide, wide }));
    }
  }
  CHECK(!plan_batch(over).has_value());
  CHECK(plan_batch(within).has_value());
}
