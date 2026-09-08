#pragma once

// Backend-neutral statement and connection contract. Every database
// channel (ODBC today, libpq and Oracle OCI later) implements these
// interfaces; the core API above this layer depends on nothing else.
//
// Placeholders in prepared SQL are always `?`; a backend rewrites them
// for its wire protocol (libpq `$1..$n`, OCI `:1..:n`). All strings
// crossing the interface are UTF-8.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

#include <uniorm/params.hpp>
#include <uniorm/types.hpp>
#include <uniorm/value.hpp>

namespace uniorm::backend {

// Status word the backend writes into each bound column's indicator
// after every fetch. The sentinels intentionally match the ODBC values
// so the ODBC adapter can hand the caller's storage straight to the
// driver manager.
inline constexpr std::int64_t null_indicator = -1;  // SQL_NULL_DATA
inline constexpr std::int64_t no_total = -4;        // SQL_NO_TOTAL

// Neutral C-buffer kinds a backend can bind a result column into.
enum class buffer_type {
  bit,
  int8,
  int16,
  int32,
  int64,
  float32,
  float64,
  chars,
  bytes,
  timestamp_parts,
  date_parts,
  time_parts
};

// Fixed-width date/time staging structs. Their layouts are pinned to the
// ODBC C structs (SQL_TIMESTAMP_STRUCT / SQL_DATE_STRUCT /
// SQL_TIME_STRUCT); the ODBC adapter static_asserts the sizes match so
// it can bind them zero-copy. Other backends fill them at fetch time.
struct timestamp_parts {
  std::int16_t year;
  std::uint16_t month;
  std::uint16_t day;
  std::uint16_t hour;
  std::uint16_t minute;
  std::uint16_t second;
  std::uint32_t fraction_ns;
};

struct date_parts {
  std::int16_t year;
  std::uint16_t month;
  std::uint16_t day;
};

struct time_parts {
  std::uint16_t hour;
  std::uint16_t minute;
  std::uint16_t second;
};

// Caller-owned result-column binding. `data` must outlive the fetch
// loop; the backend writes the value on each fetch() and records the
// byte length (or null_indicator / no_total) into *indicator.
struct column_buffer {
  buffer_type type;
  void* data;
  std::size_t capacity;  // buffer size in bytes
  std::int64_t* indicator;
};

// What a backend implementation offers. Each flag names an optional fast path
// the core can do without: it takes a slower route rather than throwing, so a
// backend that leaves every flag false is still correct.
struct capabilities {
  bool streaming;
  bool async_io;
  bool copy_protocol;
  bool notifications;
  bool columnar_batch;
};

// Direct-write batch interface. The backend allocates column buffers;
// the caller writes values into the raw buffers and indicators, then
// calls finish() to bind and prepare for execution. This avoids the
// sql_value intermediate layer for maximum throughput.
struct batch_writer_iface {
  virtual ~batch_writer_iface() = default;

  // Allocate a column buffer for `count` rows of the given type.
  // Returns the column index.
  virtual std::size_t add_column(buffer_type type, std::size_t count,
    std::size_t element_size) = 0;

  // Raw buffer access for the given column. The caller writes values
  // at data()[row * element_size()] and sets indicators()[row].
  virtual void* data(std::size_t column) = 0;
  virtual std::size_t element_size(std::size_t column) = 0;
  virtual std::int64_t* indicators(std::size_t column) = 0;

  // Bind the accumulated buffers and prepare for execution. After this
  // call, the statement can be executed with set_paramset_size + execute.
  virtual void finish() = 0;
};

struct statement_iface {
  virtual ~statement_iface() = default;

  // SQL carries `?` placeholders; the backend translates them.
  virtual void prepare(std::string_view sql) = 0;

  // index is 1-based. The backend owns all marshalling scratch
  // (indicators, date/time staging) until reset(); the value storage
  // itself (the params object) must outlive execute(), and the backend
  // may bind pointers into it zero-copy.
  virtual void bind_parameter(std::size_t index, sql_value const& value) = 0;

  // Bind all params in order (1-based indexing).
  void bind_params(params const& p) {
    for (std::size_t i = 0; i < p.size(); ++i) {
      bind_parameter(i + 1, p.at(i));
    }
  }

  virtual void bind_column(std::size_t index, column_buffer const& buffer) = 0;

  // Batch parameter binding: takes row-oriented data by const reference.
  // The backend internally converts to its preferred physical layout
  // (e.g. columnar for ODBC, pointer arrays for libpq).
  virtual void bind_batch_params(std::vector<params> const& rows) = 0;

  // Direct-write batch: returns a batch_writer that allocates column
  // buffers owned by this statement. The caller writes values directly
  // into the buffers, then calls finish() to bind. The batch_writer is
  // valid until the next reset() or prepare_batch() call.
  virtual batch_writer_iface& prepare_batch() = 0;

  virtual void execute() = 0;
  virtual bool fetch() = 0;  // false when the result set is exhausted
  virtual std::size_t affected_rows() const = 0;
  virtual std::vector<column_info> column_meta() const = 0;

  // Block fetch support: set the number of rows to fetch per SQLFetch call.
  virtual void set_row_array_size(std::size_t size) = 0;
  virtual std::size_t rows_fetched() const = 0;

  // Estimated total rows of the current result set, if the backend can
  // tell before fetching (e.g. buffered result sets). 0 when unknown;
  // callers use it only to pre-size result vectors.
  virtual std::size_t result_row_estimate() const { return 0; }

  // Batch insert support: set the number of parameter sets per SQLExecute.
  virtual void set_paramset_size(std::size_t size) = 0;

  // Continuation reads for values that did not fit the bound buffer
  // (indicator is no_total or exceeds capacity). column is 1-based.
  virtual std::string read_long_text(std::size_t column) = 0;
  virtual std::vector<std::byte> read_long_bytes(std::size_t column) = 0;

  // Cache-reuse contract: called before a cached statement is rebound for
  // the same SQL text. Closing the cursor and dropping this statement's own
  // bookkeeping suffices, since rebinding replaces each slot. A backend whose
  // driver keeps stale bindings past a rebind must clear them here.
  virtual void reset() = 0;
};

struct connection_iface {
  virtual ~connection_iface() = default;

  // Receives the scheme-specific tail of the user's connection string
  // (everything after "scheme://", or the whole string for the bare
  // DSN form).
  virtual void open(std::string_view connection_string) = 0;
  virtual void close() = 0;
  virtual bool is_open() const noexcept = 0;

  // Transaction primitives. The core drives the autocommit toggle
  // around begin/commit/rollback; backends just implement the calls.
  virtual void set_autocommit(bool enabled) = 0;
  virtual void commit() = 0;
  virtual void rollback() = 0;

  virtual capabilities caps() const noexcept = 0;

  // Database product name used for dialect detection.
  virtual std::string dbms_name() const = 0;

  virtual std::unique_ptr<statement_iface> create_statement() = 0;

  // Escape hatches (design doc 5.3): native handle of the backend
  // connection (SQLHDBC, PGconn*, OCIEnv*, ...), and typed extension
  // objects looked up by type. Both remain owned by uniorm; callers
  // must leave them self-consistent before returning control.
  virtual void* native_handle() noexcept = 0;
  virtual void* extension(std::type_index) noexcept { return nullptr; }
};

// Minimal schema-metadata extension. Backends that can introspect a
// live schema expose this via connection_iface::extension(); orm
// validation depends on it.
struct schema_metadata {
  struct column_row {
    std::string name;
    sql_type type;
    int native_type;
    bool nullable;
  };

  virtual ~schema_metadata() = default;
  virtual std::vector<column_row> table_columns(
    std::string_view table) = 0;
};

}  // namespace uniorm::backend
