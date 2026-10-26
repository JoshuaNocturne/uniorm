#include "statement.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <oci.h>
#include <orl.h>

#include <uniorm/backend/error.hpp>
#include <uniorm/detail/time.hpp>

#include "error.hpp"
#include "native_types.hpp"
#include "sql_text.hpp"

namespace uniorm::oracle_oci {

namespace {

// Widest scalar this statement stages a row of. A column the server
// describes no wider than this arrives whole in the staging, so its true
// length is always known; beyond it a value comes from a read of its own.
constexpr std::size_t max_scalar_width = 32767;

// Oracle's own fallback when a column describes no width of its own.
constexpr std::size_t unknown_scalar_width = 4096;

OraText* writable_text(std::string& value) {
  return reinterpret_cast<OraText*>(value.data());
}

OraText const* readable_text(std::string_view value) {
  return reinterpret_cast<OraText const*>(value.data());
}

char const* slot_at(column_slot const& slot, std::size_t row) {
  return slot.bytes.data() + row * slot.row_size;
}

char* caller_at(column_slot const& slot, std::size_t row) {
  return static_cast<char*>(slot.target.data) + row * slot.target.capacity;
}

// How many bytes of a staged row can be read. OCI reports the value's own
// length, which a staging buffer too small for it never held: a LONG beyond
// the widest define OCI takes is the one value this backend hands over short.
std::size_t staged_length(column_slot const& slot, std::size_t row) {
  std::size_t const length = slot.lengths[row];
  return length > slot.row_size ? slot.row_size : length;
}

bool is_date_column(ub2 dty) {
  return dty == SQLT_DAT || dty == SQLT_ODT;
}

bool is_number_column(ub2 dty) {
  return dty == SQLT_NUM;
}

// Widest decimal spelling a NUMBER of this shape can arrive as. A NUMBER's
// described DATA_SIZE is the width of its *internal* form, 22 bytes for every
// precision, so sizing a text define from it truncates a NUMBER(38) value
// whose digits run to 30 and more. Oracle's own ceiling for the widest
// NUMBER is 177 characters; a column with no scale to work from gets that.
std::size_t number_text_width(described_column const& column) {
  if (column.scale < 0 || column.precision <= 0) {
    return 177;
  }
  std::int64_t const digits = column.precision;
  std::int64_t const scale = column.scale;
  std::int64_t const whole = digits > scale ? digits - scale : 1;
  std::int64_t const width = 1 + whole + (scale > 0 ? 1 + scale : 0);
  return static_cast<std::size_t>(width);
}

bool is_timestamp_column(ub2 dty) {
  return dty == SQLT_TIMESTAMP || dty == SQLT_TIMESTAMP_TZ ||
    dty == SQLT_TIMESTAMP_LTZ;
}

// The shape OCI writes for one row, from what the caller bound and what the
// server said the column is.
stage_kind pick_stage(
  backend::buffer_type requested, described_column const& column) {
  // A BFILE locator names an OS file OCI reads only once it is opened, and
  // opening it needs the directory alias the server stored beside the path --
  // which nothing here asks for. Better this than a fetch that dies inside
  // OCILobRead with a code that says nothing about why.
  if (column.dty == SQLT_BFILEE) {
    throw oci_error("read a BFILE column", {});
  }
  if (is_locator(column.dty)) {
    return is_wide_text(column.dty) ? stage_kind::wide_text
                                    : stage_kind::wide_binary;
  }
  switch (requested) {
  case backend::buffer_type::bit:
  case backend::buffer_type::int8:
  case backend::buffer_type::int16:
  case backend::buffer_type::int32:
  case backend::buffer_type::int64:
  case backend::buffer_type::float32:
  case backend::buffer_type::float64:
    return stage_kind::number;
  case backend::buffer_type::chars:
    return stage_kind::text;
  case backend::buffer_type::bytes:
    return stage_kind::binary;
  case backend::buffer_type::timestamp_parts:
  case backend::buffer_type::date_parts:
  case backend::buffer_type::time_parts:
    if (is_date_column(column.dty)) {
      return stage_kind::date;
    }
    if (is_timestamp_column(column.dty)) {
      return stage_kind::timestamp;
    }
    throw oci_error("bind a column of type " + std::to_string(column.dty) +
        " into a date or time field",
      {});
  }
  throw oci_error("bind a result column of an unknown buffer type", {});
}

// Bytes one staged row of a text or binary column needs.
std::size_t stage_width(
  described_column const& column, std::size_t requested_capacity) {
  if (needs_wide_staging(column.dty)) {
    // A LONG, or an NCLOB fetched as plain text, truncates to the buffer it
    // was defined with and its described width says nothing about the value,
    // so offer the widest buffer OCI takes; a row beyond that is the
    // read_long_text() fallback's problem.
    return max_scalar_width > requested_capacity ? max_scalar_width
                                                 : requested_capacity;
  }
  if (is_number_column(column.dty)) {
    std::size_t const width = number_text_width(column);
    return width > max_scalar_width ? max_scalar_width : width;
  }
  if (column.byte_size == 0) {
    return unknown_scalar_width > requested_capacity
      ? unknown_scalar_width
      : requested_capacity;
  }
  return column.byte_size > max_scalar_width ? max_scalar_width
                                             : column.byte_size;
}

sb8 number_to_integer(OCINumber const& number, OCIError* error) {
  sb8 out = 0;
  throw_if_error(
    OCINumberToInt(error, &number, sizeof(out), OCI_NUMBER_SIGNED, &out),
    error, "read a NUMBER as an integer");
  return out;
}

double number_to_real(OCINumber const& number, OCIError* error) {
  double out = 0;
  throw_if_error(
    OCINumberToReal(error, &number, sizeof(out), &out), error,
    "read a NUMBER as a real");
  return out;
}

std::string number_to_text(
  OCINumber const& number, OCIError* error) {
  // TM: the shortest spelling that reads back as the same value, with no
  // group separator and no currency, which is what a column's text should be.
  // The buffer is Oracle's own ceiling for a NUMBER's text -- a value like
  // 1E-80 spells 81 characters long -- because OCINumberToText writes the
  // whole spelling and reports its length, taking no notice of the size it was
  // handed.
  char buffer[178] = {};
  ub4 size = static_cast<ub4>(sizeof(buffer) - 1);
  throw_if_error(OCINumberToText(error, &number,
                   readable_text("TM"), 2, nullptr, 0, &size,
                   reinterpret_cast<OraText*>(buffer)),
    error, "read a NUMBER as text");
  if (size > sizeof(buffer) - 1) {
    throw oci_error("read a NUMBER whose text runs past " +
      std::to_string(size) + " characters", {});
  }
  return std::string(buffer, size);
}

void integer_from_value(sql_value const& value, OCIError* error,
  OCINumber& number) {
  std::int64_t whole = 0;
  std::visit(
    [&whole](auto const& held) {
      using Held = std::remove_cvref_t<decltype(held)>;
      if constexpr (std::is_same_v<Held, bool>) {
        whole = held ? 1 : 0;
      } else if constexpr (std::is_integral_v<Held>) {
        whole = static_cast<std::int64_t>(held);
      }
    },
    value);
  throw_if_error(OCINumberFromInt(error, &whole, sizeof(whole),
                   OCI_NUMBER_SIGNED, &number),
    error, "bind an integer parameter");
}

void real_from_value(double held, OCIError* error, OCINumber& number) {
  throw_if_error(OCINumberFromReal(error, &held, sizeof(held), &number), error,
    "bind a floating point parameter");
}

// The descriptor kind a datetime column's rows must be allocated as, from the
// type the server said the column is. The three codes are unrelated to the
// SQLT_ numbers beside them.
ub4 datetime_kind_of(ub2 column_type) {
  switch (column_type) {
  case SQLT_TIMESTAMP_TZ:
    return OCI_DTYPE_TIMESTAMP_TZ;
  case SQLT_TIMESTAMP_LTZ:
    return OCI_DTYPE_TIMESTAMP_LTZ;
  default:
    return OCI_DTYPE_TIMESTAMP;
  }
}

}  // namespace

oci_statement::oci_statement(oracle_oci::connection& conn)
  : conn_(conn), stmt_(conn.env(), "statement") {}

oci_statement::~oci_statement() = default;

void oci_statement::prepare(std::string_view sql) {
  if (conn_.service() == nullptr) {
    throw oci_error("prepare a statement on a session that is not open", {});
  }
  std::size_t placeholders = 0;
  rewritten_ = rewrite_placeholders(sql, placeholders);
  query_ = leads_with_query(sql);
  // OCIStmtPrepare, not OCIStmtPrepare2: the second keeps a server-side
  // statement cache keyed by a cursor name, and this library's own cache
  // already holds the handle to reuse.
  throw_if_error(OCIStmtPrepare(stmt_.get(), conn_.error(),
                   readable_text(rewritten_),
                   static_cast<ub4>(rewritten_.size()), OCI_NTV_SYNTAX,
                   OCI_DEFAULT),
    conn_.error(), "prepare statement");
}

void oci_statement::bind_parameter(
  std::size_t index, sql_value const& value) {
  while (params_.size() < index) {
    params_.emplace_back();
  }
  bind_one(index, value);
}

void oci_statement::bind_one(std::size_t index, sql_value const& value) {
  param_slot& slot = params_[index - 1];
  slot = param_slot();
  slot.value = nullptr;
  slot.size = 0;
  slot.dty = SQLT_VNU;
  slot.indicator = 0;

  if (is_null(value)) {
    slot.indicator = -1;
    slot.dty = SQLT_AFC;
  } else if (std::holds_alternative<double>(value)) {
    real_from_value(std::get<double>(value), conn_.error(), slot.number);
    slot.value = &slot.number;
    slot.size = sizeof(OCINumber);
  } else if (std::holds_alternative<std::string>(value)) {
    slot.text = std::get<std::string>(value);
    slot.dty = SQLT_CHR;
    slot.value = writable_text(slot.text);
    slot.size = static_cast<sb4>(slot.text.size());
  } else if (std::holds_alternative<std::vector<std::byte>>(value)) {
    slot.bytes = std::get<std::vector<std::byte>>(value);
    slot.dty = SQLT_BIN;
    slot.value = slot.bytes.data();
    slot.size = static_cast<sb4>(slot.bytes.size());
  } else if (std::holds_alternative<timestamp>(value)) {
    auto const parts =
      uniorm::detail::break_timestamp(std::get<timestamp>(value));
    slot.stamp = detail::timestamp_descriptor(
      conn_.env(), "bind a timestamp parameter");
    throw_if_error(OCIDateTimeConstruct(conn_.env(), conn_.error(),
                     slot.stamp.get(), static_cast<sb2>(parts.year),
                     static_cast<ub1>(parts.month),
                     static_cast<ub1>(parts.day),
                     static_cast<ub1>(parts.hour),
                     static_cast<ub1>(parts.minute),
                     static_cast<ub1>(parts.second),
                     static_cast<ub4>(parts.fraction_ns), nullptr, 0),
      conn_.error(), "build a timestamp to bind");
    slot.dty = SQLT_TIMESTAMP;
    slot.value = slot.stamp.address();
    slot.size = static_cast<sb4>(sizeof(OCIDateTime*));
  } else {
    integer_from_value(value, conn_.error(), slot.number);
    slot.value = &slot.number;
    slot.size = sizeof(OCINumber);
  }

  OCIBind* handle = nullptr;
  throw_if_error(OCIBindByPos(stmt_.get(), &handle, conn_.error(),
                   static_cast<ub4>(index), slot.value, slot.size, slot.dty,
                   &slot.indicator, nullptr, nullptr, 0, nullptr, OCI_DEFAULT),
    conn_.error(), "bind parameter " + std::to_string(index));
  slot.bind = detail::bind_handle::adopt(handle);
}

void oci_statement::bind_column(
  std::size_t index, backend::column_buffer const& buffer) {
  if (!metadata_read_) {
    throw oci_error(
      "bind a result column of a statement that was never described", {});
  }
  if (index == 0 || index > described_.size()) {
    throw oci_error("result column " + std::to_string(index) +
      " is past the select list", {});
  }
  // The core binds its fields in whatever order it mapped them, so a column
  // may arrive at any point in the run -- twice may not.
  for (column_slot const& slot : slots_) {
    if (slot.position == index) {
      throw oci_error("bind result column " + std::to_string(index) +
        " a second time", {});
    }
  }
  slots_.emplace_back();
  column_slot& slot = slots_.back();
  slot.position = index;
  slot.target = buffer;
  slot.described = described_[index - 1];
  slot.kind = pick_stage(buffer.type, slot.described);

  std::size_t const rows = row_array_size_;
  slot.nulls.assign(rows, 0);
  slot.lengths.assign(rows, 0);
  switch (slot.kind) {
  case stage_kind::number:
    slot.numbers.resize(rows);
    break;
  case stage_kind::date:
    slot.dates.resize(rows);
    break;
  case stage_kind::text:
  case stage_kind::binary:
    slot.row_size = stage_width(slot.described, buffer.capacity);
    slot.bytes.resize(rows * slot.row_size);
    break;
  case stage_kind::timestamp: {
    // One datetime descriptor per row: OCI hands a TIMESTAMP array as an
    // array of pointers to them, and the pointers must exist before the
    // define does.
    ub4 const kind = datetime_kind_of(slot.described.dty);
    for (std::size_t row = 0; row < rows; ++row) {
      slot.stamps.add(conn_.env(), kind, "fetch a timestamp column");
    }
    break;
  }
  case stage_kind::wide_text:
  case stage_kind::wide_binary:
    slot.lobs.reserve(rows);
    slot.locator_slots.reserve(rows);
    for (std::size_t row = 0; row < rows; ++row) {
      slot.lobs.emplace_back(conn_.env(), "fetch a LOB column");
      slot.locator_slots.push_back(slot.lobs.back().get());
    }
    break;
  }
  apply_define(index, slot);
}

void oci_statement::apply_define(std::size_t position, column_slot& slot) {
  void* value = nullptr;
  sb4 size = 0;
  ub2 dty = SQLT_CHR;
  switch (slot.kind) {
  case stage_kind::number:
    value = slot.numbers.data();
    size = sizeof(OCINumber);
    dty = SQLT_VNU;
    break;
  case stage_kind::date:
    value = slot.dates.data();
    size = sizeof(OCIDate);
    dty = SQLT_ODT;
    break;
  case stage_kind::text:
    value = slot.bytes.data();
    size = static_cast<sb4>(slot.row_size);
    dty = SQLT_CHR;
    break;
  case stage_kind::binary:
    value = slot.bytes.data();
    size = static_cast<sb4>(slot.row_size);
    dty = SQLT_BIN;
    break;
  case stage_kind::timestamp:
    value = slot.stamps.data();
    size = sizeof(OCIDateTime*);
    dty = slot.described.dty;
    break;
  case stage_kind::wide_text:
  case stage_kind::wide_binary:
    value = slot.locator_slots.data();
    size = sizeof(OCILobLocator*);
    dty = slot.described.dty;
    break;
  }

  OCIDefine* handle = nullptr;
  throw_if_error(OCIDefineByPos(stmt_.get(), &handle, conn_.error(),
                   static_cast<ub4>(position), value, size, dty,
                   slot.nulls.data(), slot.lengths.data(), nullptr,
                   OCI_DEFAULT),
    conn_.error(), "define result column " + std::to_string(position));
  slot.define = detail::define_handle::adopt(handle);
  // The strides that turn one define into an array of them: the value steps
  // by the row's own size, the two status arrays by their element widths.
  throw_if_error(OCIDefineArrayOfStruct(handle, conn_.error(),
                   static_cast<ub4>(size), sizeof(sb2), sizeof(ub2), 0),
    conn_.error(), "array-define result column " + std::to_string(position));
}

void oci_statement::describe() {
  run(OCI_DESCRIBE_ONLY);
  awaiting_execute_ = true;

  described_.clear();
  // The count the statement reports for its own select list. Asking
  // OCIParamGet for one position past it answers OCI_ERROR with ORA-24334,
  // not OCI_NO_DATA, so walking until it refuses cannot tell an exhausted
  // list from a describe that genuinely failed.
  ub4 columns = 0;
  throw_if_error(OCIAttrGet(stmt_.get(), OCI_HTYPE_STMT, &columns, nullptr,
                   OCI_ATTR_PARAM_COUNT, conn_.error()),
    conn_.error(), "count the select-list columns");
  for (ub4 position = 1; position <= columns; ++position) {
    OCIParam* parameter = nullptr;
    throw_if_error(OCIParamGet(stmt_.get(), OCI_HTYPE_STMT,
                     conn_.error(),
                     reinterpret_cast<void**>(&parameter), position),
      conn_.error(), "describe a select-list column");
    auto owned_parameter = detail::param_handle::adopt(parameter);

    described_column column;
    ub4 dty = 0;
    throw_if_error(OCIAttrGet(parameter, OCI_DTYPE_PARAM, &dty, nullptr,
                     OCI_ATTR_DATA_TYPE, conn_.error()),
      conn_.error(), "read a described column's type");
    column.dty = static_cast<ub2>(dty);

    ub4 byte_size = 0;
    OCIAttrGet(parameter, OCI_DTYPE_PARAM, &byte_size, nullptr,
      OCI_ATTR_DATA_SIZE, conn_.error());
    column.byte_size = byte_size;

    sb4 precision = 0;
    OCIAttrGet(parameter, OCI_DTYPE_PARAM, &precision, nullptr,
      OCI_ATTR_PRECISION, conn_.error());
    column.precision = precision;

    sb4 scale = 0;
    OCIAttrGet(parameter, OCI_DTYPE_PARAM, &scale, nullptr, OCI_ATTR_SCALE,
      conn_.error());
    column.scale = scale;

    // A select-list item describes no nullability of its own through OCI, and
    // the safe guess is the one that costs nothing: the core reads a column as
    // nullable whether or not the server said so.
    column.nullable = true;

    // OCI answers with a pointer into its own describe memory and the length
    // beside it; handing it a caller buffer writes the address into the
    // buffer's bytes instead.
    OraText const* name = nullptr;
    ub4 name_length = 0;
    if (OCIAttrGet(parameter, OCI_DTYPE_PARAM, &name, &name_length,
          OCI_ATTR_NAME, conn_.error()) == OCI_SUCCESS && name != nullptr) {
      column.name.assign(reinterpret_cast<char const*>(name), name_length);
    }
    described_.push_back(std::move(column));
  }
  metadata_read_ = true;
}

std::size_t oci_statement::run(ub4 mode, ub4 iterations) {
  throw_if_error(OCIStmtExecute(conn_.service(), stmt_.get(), conn_.error(),
    iterations, 0, nullptr, nullptr, mode),
    conn_.error(), "execute statement");
  return mode == OCI_DESCRIBE_ONLY ? 0 : row_count();
}

void oci_statement::run_batch() {
  // Swapped out before the loop: a row that throws must not leave the batch on
  // the statement, or the next execute this cached handle runs replays it.
  std::vector<params> rows;
  rows.swap(batch_);
  // One commit for the batch rather than one per row. OCI_COMMIT_ON_SUCCESS
  // costs a round trip the row does not otherwise need, and a batch is a
  // thousand rows through this loop; committing the rows that ran, when one
  // fails partway, is what each row committing on its own would have left
  // behind the server with.
  std::size_t ran = 0;
  affected_ = 0;
  try {
    for (auto const& row : rows) {
      params_.clear();
      while (params_.size() < row.size()) {
        params_.emplace_back();
      }
      for (std::size_t index = 0; index < row.size(); ++index) {
        bind_one(index + 1, row.at(index));
      }
      affected_ += run(OCI_DEFAULT);
      ++ran;
    }
  } catch (...) {
    if (conn_.autocommit() && ran > 0) {
      try {
        conn_.commit();
      } catch (...) {
      }
    }
    throw;
  }
  if (conn_.autocommit()) {
    conn_.commit();
  }
}

std::size_t oci_statement::statement_count(ub4 attribute) {
  ub4 count = 0;
  if (OCIAttrGet(stmt_.get(), OCI_HTYPE_STMT, &count, nullptr, attribute,
        conn_.error()) != OCI_SUCCESS) {
    return 0;
  }
  return static_cast<std::size_t>(count);
}

// Rows the write this statement ran touched.
std::size_t oci_statement::row_count() {
  return statement_count(OCI_ATTR_ROW_COUNT);
}

// Rows the last fetch put into the array. OCI_ATTR_ROW_COUNT keeps a running
// total across fetches, which would count a block twice.
std::size_t oci_statement::block_rows() {
  return statement_count(OCI_ATTR_ROWS_FETCHED);
}

void oci_statement::execute() {
  exhausted_ = false;
  affected_ = 0;
  if (query_) {
    describe();
    return;
  }
  described_.clear();
  metadata_read_ = false;
  if (!batch_.empty()) {
    run_batch();
    return;
  }
  affected_ = run(conn_.autocommit() ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT);
}

bool oci_statement::fetch() {
  if (awaiting_execute_) {
    awaiting_execute_ = false;
    // Iterating once would execute the query *and* pull its first row into
    // the defines, and that row would never reach OCIStmtFetch2.
    run(conn_.autocommit() ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT, 0);
  }
  if (exhausted_) {
    rows_fetched_ = 0;
    return false;
  }
  sword const status = OCIStmtFetch2(stmt_.get(), conn_.error(),
    static_cast<ub4>(row_array_size_), OCI_FETCH_NEXT, 0, OCI_DEFAULT);
  // A block smaller than the row array arrives with this status: it says the
  // cursor is spent, not that the block is empty. Asking it again is an error.
  if (status == OCI_NO_DATA) {
    exhausted_ = true;
    rows_fetched_ = block_rows();
    if (rows_fetched_ == 0) {
      return false;
    }
    materialize(rows_fetched_);
    return true;
  }
  throw_if_error(status, conn_.error(), "fetch rows");
  rows_fetched_ = block_rows();
  if (rows_fetched_ == 0) {
    return false;
  }
  materialize(rows_fetched_);
  return true;
}

void oci_statement::materialize(std::size_t rows) {
  for (column_slot& slot : slots_) {
    slot.wide_rows.assign(rows, 0);
    for (std::size_t row = 0; row < rows; ++row) {
      if (slot.nulls[row] == -1) {
        slot.target.indicator[row] = backend::null_indicator;
        continue;
      }
      slot.target.indicator[row] = 0;
      fill_cell(slot, row);
    }
  }
}

void oci_statement::fill_cell(column_slot& slot, std::size_t row) {
  switch (slot.kind) {
  case stage_kind::number:
    put_number(slot, row);
    return;
  case stage_kind::text:
    put_text(slot, row, slot_at(slot, row), staged_length(slot, row));
    return;
  case stage_kind::binary:
    put_text(slot, row, slot_at(slot, row), staged_length(slot, row));
    return;
  case stage_kind::date:
    put_date(slot, row);
    return;
  case stage_kind::timestamp:
    put_timestamp(slot, row);
    return;
  case stage_kind::wide_text:
  case stage_kind::wide_binary:
    // The value stayed behind its locator: the caller's own truncation
    // signal sends it to a read of this row.
    slot.wide_rows[row] = 1;
    slot.target.indicator[row] = backend::no_total;
    return;
  }
}

// Copies one staged text value into the caller's row slot and records the
// length the caller's convention reads back. A character binding treats a
// value as truncated one byte earlier than a binary one does, because the
// string binding reserves its last byte.
void oci_statement::put_text(
  column_slot& slot, std::size_t row, char const* data, std::size_t length) {
  // The contract lets a caller hand over a zero-length buffer; subtracting
  // from an unsigned capacity would then read the whole value as fitting.
  std::size_t const capacity = slot.target.capacity;
  bool const binary = slot.target.type == backend::buffer_type::bytes;
  std::size_t const limit =
    binary ? capacity : (capacity > 0 ? capacity - 1 : 0);
  if (length <= limit) {
    std::memcpy(caller_at(slot, row), data, length);
    slot.target.indicator[row] = static_cast<std::int64_t>(length);
    return;
  }
  std::memcpy(caller_at(slot, row), data, limit);
  slot.target.indicator[row] = static_cast<std::int64_t>(limit + 1);
  slot.wide_rows[row] = 1;
}

void oci_statement::put_number(column_slot& slot, std::size_t row) {
  OCINumber const& number = slot.numbers[row];
  char* destination = caller_at(slot, row);
  switch (slot.target.type) {
  case backend::buffer_type::bit:
  case backend::buffer_type::int8:
    *reinterpret_cast<std::int8_t*>(destination) =
      static_cast<std::int8_t>(number_to_integer(number, conn_.error()));
    return;
  case backend::buffer_type::int16:
    *reinterpret_cast<std::int16_t*>(destination) =
      static_cast<std::int16_t>(number_to_integer(number, conn_.error()));
    return;
  case backend::buffer_type::int32:
    *reinterpret_cast<std::int32_t*>(destination) =
      static_cast<std::int32_t>(number_to_integer(number, conn_.error()));
    return;
  case backend::buffer_type::int64:
    *reinterpret_cast<std::int64_t*>(destination) =
      static_cast<std::int64_t>(number_to_integer(number, conn_.error()));
    return;
  case backend::buffer_type::float32:
    *reinterpret_cast<float*>(destination) =
      static_cast<float>(number_to_real(number, conn_.error()));
    return;
  case backend::buffer_type::float64:
    *reinterpret_cast<double*>(destination) =
      number_to_real(number, conn_.error());
    return;
  case backend::buffer_type::chars:
  case backend::buffer_type::bytes: {
    std::string const text = number_to_text(number, conn_.error());
    put_text(
      slot, row, text.data(), static_cast<std::size_t>(text.size()));
    return;
  }
  case backend::buffer_type::timestamp_parts:
  case backend::buffer_type::date_parts:
  case backend::buffer_type::time_parts:
    throw oci_error("bind a NUMBER column into a date field", {});
  }
}

void oci_statement::put_date(column_slot& slot, std::size_t row) {
  OCIDate const& value = slot.dates[row];
  sb2 year = 0;
  ub1 month = 0;
  ub1 day = 0;
  ub1 hour = 0;
  ub1 minute = 0;
  ub1 second = 0;
  OCIDateGetDate(&value, &year, &month, &day);
  OCIDateGetTime(&value, &hour, &minute, &second);
  char* destination = caller_at(slot, row);
  switch (slot.target.type) {
  case backend::buffer_type::date_parts: {
    auto* parts = reinterpret_cast<backend::date_parts*>(destination);
    parts->year = year;
    parts->month = month;
    parts->day = day;
    return;
  }
  case backend::buffer_type::time_parts: {
    auto* parts = reinterpret_cast<backend::time_parts*>(destination);
    parts->hour = hour;
    parts->minute = minute;
    parts->second = second;
    return;
  }
  case backend::buffer_type::timestamp_parts: {
    auto* parts =
      reinterpret_cast<backend::timestamp_parts*>(destination);
    parts->year = year;
    parts->month = month;
    parts->day = day;
    parts->hour = hour;
    parts->minute = minute;
    parts->second = second;
    parts->fraction_ns = 0;
    return;
  }
  default:
    // A field that wants text was staged as text, not as a date: pick_stage()
    // reaches this shape only for a date, time or timestamp field.
    throw oci_error("bind a DATE column into a field that is not a date",
      {});
  }
}

void oci_statement::put_timestamp(column_slot& slot, std::size_t row) {
  OCIDateTime* value = slot.stamps.at(row);
  sb2 year = 0;
  ub1 month = 0;
  ub1 day = 0;
  ub1 hour = 0;
  ub1 minute = 0;
  ub1 second = 0;
  ub4 fraction = 0;
  throw_if_error(
    OCIDateTimeGetDate(conn_.env(), conn_.error(), value, &year, &month, &day),
    conn_.error(), "read a TIMESTAMP column's date");
  throw_if_error(OCIDateTimeGetTime(
                     conn_.env(), conn_.error(), value, &hour, &minute, &second,
                     &fraction),
    conn_.error(), "read a TIMESTAMP column's time");
  char* destination = caller_at(slot, row);
  switch (slot.target.type) {
  case backend::buffer_type::date_parts: {
    auto* parts = reinterpret_cast<backend::date_parts*>(destination);
    parts->year = year;
    parts->month = month;
    parts->day = day;
    return;
  }
  case backend::buffer_type::time_parts: {
    auto* parts = reinterpret_cast<backend::time_parts*>(destination);
    parts->hour = hour;
    parts->minute = minute;
    parts->second = second;
    return;
  }
  case backend::buffer_type::timestamp_parts: {
    auto* parts =
      reinterpret_cast<backend::timestamp_parts*>(destination);
    parts->year = year;
    parts->month = month;
    parts->day = day;
    parts->hour = hour;
    parts->minute = minute;
    parts->second = second;
    parts->fraction_ns = fraction;
    return;
  }
  default:
    throw oci_error(
      "bind a TIMESTAMP column into a field that is not a date", {});
  }
}

std::size_t oci_statement::affected_rows() const {
  return affected_;
}

std::vector<column_info> oci_statement::column_meta() const {
  std::vector<column_info> out;
  out.reserve(described_.size());
  for (auto const& column : described_) {
    column_info info;
    info.name = column.name;
    info.type =
      sql_type_from_native(column.dty, column.precision, column.scale);
    info.display_size = info.type == sql_type::decimal
      ? static_cast<std::size_t>(column.precision)
      : column.byte_size;
    info.scale = static_cast<std::size_t>(column.scale < 0 ? 0
                                                           : column.scale);
    info.nullable = column.nullable;
    out.push_back(std::move(info));
  }
  return out;
}

void oci_statement::set_row_array_size(std::size_t size) {
  row_array_size_ = size == 0 ? 1 : size;
}

std::size_t oci_statement::rows_fetched() const {
  return rows_fetched_;
}

void oci_statement::set_paramset_size(std::size_t) {
  // Ignored: bind_batch_params() hands over the whole batch and run_batch()
  // walks it, so the count needs no second telling.
}

void oci_statement::bind_batch_params(std::vector<params> const& rows) {
  // Row by row rather than batch_ = rows: params' variadic constructor
  // outranks its implicit copy for a non-const lvalue, and a vector
  // assignment reaches for it.
  batch_.clear();
  batch_.reserve(rows.size());
  for (params const& row : rows) {
    batch_.push_back(row);
  }
}

backend::batch_writer_iface& oci_statement::prepare_batch() {
  throw backend::capability_not_supported(
    "direct-write batch binding is not supported");
}

column_slot& oci_statement::wide_slot(std::size_t column) {
  for (column_slot& slot : slots_) {
    if (slot.position == column) {
      return slot;
    }
  }
  throw oci_error(
    "read a value of result column " + std::to_string(column), {});
}

void oci_statement::require_wide(
  column_slot& slot, std::size_t column, std::size_t row) {
  if (row >= slot.wide_rows.size() || slot.wide_rows[row] == 0) {
    throw oci_error("read column " + std::to_string(column) +
      " of row " + std::to_string(row) + ", which owes no such read", {});
  }
}

std::string oci_statement::read_long_text(std::size_t column, std::size_t row) {
  column_slot& slot = wide_slot(column);
  require_wide(slot, column, row);
  if (slot.kind == stage_kind::wide_text) {
    return read_locator_text(slot, row);
  }
  if (slot.kind == stage_kind::number) {
    return number_to_text(slot.numbers[row], conn_.error());
  }
  // A scalar whose value did not fit the caller's slot is whole in the
  // staging it was fetched into, unless the server truncated that too.
  return std::string(
    slot_at(slot, row), staged_length(slot, row));
}

std::vector<std::byte> oci_statement::read_long_bytes(
  std::size_t column, std::size_t row) {
  column_slot& slot = wide_slot(column);
  require_wide(slot, column, row);
  if (slot.kind == stage_kind::wide_binary) {
    return read_locator_bytes(slot, row);
  }
  if (slot.kind == stage_kind::number) {
    std::string const text = number_to_text(slot.numbers[row], conn_.error());
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
  }
  std::vector<std::byte> out(staged_length(slot, row));
  std::memcpy(out.data(), slot_at(slot, row), out.size());
  return out;
}

std::string oci_statement::read_locator_text(
  column_slot& slot, std::size_t row) {
  // Read as bytes, with no character set given: the amount OCI reports is then
  // in bytes too, which is the only way to know how much of the buffer holds a
  // value. The cost is that OCI does no conversion either, so this hands over
  // the value in the charset the LOB was stored in -- UTF-8 for the AL32UTF8
  // database every other path here assumes. An NCLOB is not fetched this way
  // at all: it comes with the row, converted by OCI to the environment's
  // charset.
  OCILobLocator* locator = slot.locator_slots[row];
  std::string out;
  ub4 offset = 1;
  for (;;) {
    char chunk[4096];
    ub4 amount = static_cast<ub4>(sizeof(chunk));
    sword const status = OCILobRead(conn_.service(), conn_.error(), locator,
      &amount, offset, chunk, static_cast<ub4>(sizeof(chunk)), nullptr,
      nullptr, 0, 0);
    throw_if_error(
      status, conn_.error(), "read a character LOB", OCI_NEED_DATA);
    out.append(chunk, amount);
    // A round that transfers nothing is the only end of the value. OCI also
    // answers OCI_SUCCESS at the edge of its prefetch region and keeps
    // streaming on NEED_DATA rounds after that.
    if (amount == 0) {
      return out;
    }
    offset += amount;
  }
}

std::vector<std::byte> oci_statement::read_locator_bytes(
  column_slot& slot, std::size_t row) {
  OCILobLocator* locator = slot.locator_slots[row];
  std::vector<std::byte> out;
  ub4 offset = 1;
  for (;;) {
    std::byte chunk[4096];
    ub4 amount = static_cast<ub4>(sizeof(chunk));
    sword const status = OCILobRead(conn_.service(), conn_.error(), locator,
      &amount, offset, chunk, static_cast<ub4>(sizeof(chunk)), nullptr,
      nullptr, 0, 0);
    throw_if_error(
      status, conn_.error(), "read a binary LOB", OCI_NEED_DATA);
    out.insert(out.end(), chunk, chunk + amount);
    if (amount == 0) {
      return out;
    }
    offset += amount;
  }
}

void oci_statement::reset() {
  // OCI keeps a define or bind attached to the statement until its handle
  // goes, so a rebind that followed without this would fetch into buffers
  // the previous caller still owns.
  slots_.clear();
  params_.clear();
  batch_.clear();
  described_.clear();
  metadata_read_ = false;
  awaiting_execute_ = false;
  exhausted_ = false;
  rows_fetched_ = 0;
  affected_ = 0;
}

}  // namespace uniorm::oracle_oci
