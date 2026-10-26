#pragma once

// Private header: the OCI statement, which is the backend contract's
// statement implemented over one session. Not installed.
//
// The core binds result columns only after it has executed the statement,
// and it hands each column an array of caller slots to fill. OCI describes a
// query into the statement handle, so this statement runs a SELECT in two
// passes: a describe-only execute answers the metadata read, and the first
// fetch performs the real execute. Between them every column stages into
// memory this statement owns, because OCI reports a fetched value's length
// and NULL in two-byte arrays while the contract asks for one eight-byte
// array per row.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <uniorm/backend/backend.hpp>

#include "connection.hpp"
#include "handles.hpp"

namespace uniorm::oracle_oci {

// What OCI writes for one row of a bound column, before it reaches the
// caller's slot.
enum class stage_kind {
  number,        // an OCINumber, then converted to the caller's width
  text,          // bytes, one row-sized slot each
  binary,        // bytes, one row-sized slot each
  date,          // an OCIDate
  timestamp,     // a datetime descriptor per row
  wide_text,     // a locator per row; the value only a read of its own gives
  wide_binary
};

// A select-list item as the server described it.
struct described_column {
  std::string name;
  ub2 dty = SQLT_CHR;
  std::size_t byte_size = 0;
  std::int32_t precision = 0;
  std::int32_t scale = 0;
  bool nullable = true;
};

// One caller binding, the staging behind it, and the rows whose values still
// owe a read of their own.
struct column_slot {
  backend::column_buffer target;
  stage_kind kind = stage_kind::text;
  described_column described;
  // The 1-based select-list item this binding was for. The core binds in its
  // own order, which need not be this one, and a continuation read names the
  // column rather than the arrival.
  std::size_t position = 0;

  std::vector<OCINumber> numbers;
  std::vector<OCIDate> dates;
  std::vector<char> bytes;
  std::size_t row_size = 0;

  // OCI's own per-row status arrays, translated into target.indicator.
  std::vector<sb2> nulls;
  std::vector<ub2> lengths;

  // Rows whose value did not reach the caller's slot whole, one flag each in
  // fetch order: one behind a locator, one truncated to a slot too narrow for
  // it. A continuation read names its row, so reading one twice repeats the
  // value and a block can be walked in any order.
  std::vector<unsigned char> wide_rows;

  std::vector<detail::lob_descriptor> lobs;
  std::vector<OCILobLocator*> locator_slots;
  detail::datetime_staging stamps;

  detail::define_handle define;
};

// A bound parameter, held in the shape OCI binds it in. The value pointers
// handed to OCI must stay put until the execute, which is why these live in
// a deque: appending another parameter must not relocate the earlier ones.
struct param_slot {
  OCINumber number{};
  std::string text;
  std::vector<std::byte> bytes;
  detail::timestamp_descriptor stamp;
  sb2 indicator = 0;  // -1 when the value is NULL
  void* value = nullptr;
  sb4 size = 0;
  ub2 dty = SQLT_VNU;
  detail::bind_handle bind;
};

class UNIORM_ORACLE_OCI_API oci_statement : public backend::statement_iface {
public:
  explicit oci_statement(oracle_oci::connection& conn);
  ~oci_statement() override;

  oci_statement(oci_statement const&) = delete;
  oci_statement& operator=(oci_statement const&) = delete;

  void prepare(std::string_view sql) override;
  void bind_parameter(std::size_t index, sql_value const& value) override;
  void bind_column(
    std::size_t index, backend::column_buffer const& buffer) override;
  void bind_batch_params(std::vector<params> const& rows) override;
  backend::batch_writer_iface& prepare_batch() override;
  void execute() override;
  bool fetch() override;
  std::size_t affected_rows() const override;
  std::vector<column_info> column_meta() const override;
  void set_row_array_size(std::size_t size) override;
  std::size_t rows_fetched() const override;
  void set_paramset_size(std::size_t size) override;
  std::string read_long_text(std::size_t column, std::size_t row) override;
  std::vector<std::byte> read_long_bytes(
    std::size_t column, std::size_t row) override;
  void reset() override;

private:
  void describe();
  // One OCIStmtExecute, returning the rows it touched. A query is executed
  // with iterations 0: asking for one would fetch its first row before any
  // OCIStmtFetch2 could see it.
  std::size_t run(ub4 mode, ub4 iterations = 1);
  void run_batch();
  // A count OCI keeps on the statement handle: the rows a write affected, or
  // the rows one fetch put into the array.
  std::size_t statement_count(ub4 attribute);
  std::size_t row_count();
  std::size_t block_rows();
  void apply_define(std::size_t position, column_slot& slot);
  void materialize(std::size_t rows);
  void fill_cell(column_slot& slot, std::size_t row);
  void put_text(column_slot& slot, std::size_t row, char const* data,
    std::size_t length);
  void put_number(column_slot& slot, std::size_t row);
  void put_date(column_slot& slot, std::size_t row);
  void put_timestamp(column_slot& slot, std::size_t row);
  std::string read_locator_text(column_slot& slot, std::size_t row);
  std::vector<std::byte> read_locator_bytes(column_slot& slot, std::size_t row);
  column_slot& wide_slot(std::size_t column);
  void require_wide(column_slot& slot, std::size_t column, std::size_t row);
  void bind_one(std::size_t index, sql_value const& value);

  oracle_oci::connection& conn_;
  detail::stmt_handle stmt_;
  // The text handed to the server: placeholders rewritten to :1..:n. OCI
  // keeps no copy of it once caching is off, so this outlives the binds.
  std::string rewritten_;
  std::deque<param_slot> params_;
  std::vector<described_column> described_;
  std::vector<column_slot> slots_;
  // A batch the row-wise channel handed over: one execute per row, since
  // this backend reports no columnar batch and no array row count.
  std::vector<params> batch_;

  std::size_t row_array_size_ = 1;
  std::size_t rows_fetched_ = 0;
  std::size_t affected_ = 0;
  bool query_ = false;
  bool metadata_read_ = false;
  // The describe pass is done and the real execute has not: the first fetch
  // owes it.
  bool awaiting_execute_ = false;
  // The cursor answered OCI_NO_DATA. Asking it again is an error, not a
  // second empty block.
  bool exhausted_ = false;
};

}  // namespace uniorm::oracle_oci
