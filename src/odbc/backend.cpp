#include "backend.hpp"

#include <algorithm>
#include <cstring>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include <sql.h>
#include <sqlext.h>

#include <uniorm/backend/error.hpp>
#include <uniorm/backend/registry.hpp>

#include "uniorm/detail/time.hpp"
#include "environment.hpp"
#include "error.hpp"
#include "native_types.hpp"

namespace uniorm::odbc {

namespace {

static_assert(sizeof(std::int64_t) == sizeof(SQLLEN));
static_assert(
  sizeof(backend::timestamp_parts) == sizeof(SQL_TIMESTAMP_STRUCT));
static_assert(sizeof(backend::date_parts) == sizeof(SQL_DATE_STRUCT));
static_assert(sizeof(backend::time_parts) == sizeof(SQL_TIME_STRUCT));

SQLSMALLINT c_type_for(backend::buffer_type t) {
  switch (t) {
  case backend::buffer_type::bit:
    return SQL_C_BIT;
  case backend::buffer_type::int8:
    return SQL_C_STINYINT;
  case backend::buffer_type::int16:
    return SQL_C_SSHORT;
  case backend::buffer_type::int32:
    return SQL_C_SLONG;
  case backend::buffer_type::int64:
    return SQL_C_SBIGINT;
  case backend::buffer_type::float32:
    return SQL_C_FLOAT;
  case backend::buffer_type::float64:
    return SQL_C_DOUBLE;
  case backend::buffer_type::chars:
    return SQL_C_CHAR;
  case backend::buffer_type::bytes:
    return SQL_C_BINARY;
  case backend::buffer_type::timestamp_parts:
    return SQL_C_TYPE_TIMESTAMP;
  case backend::buffer_type::date_parts:
    return SQL_C_TYPE_DATE;
  case backend::buffer_type::time_parts:
    return SQL_C_TYPE_TIME;
  }
  throw backend::backend_error("odbc", "unsupported buffer type", {});
}

std::string get_data_char(odbc::statement& stmt, SQLUSMALLINT column) {
  std::string out;
  for (;;) {
    char chunk[4096];
    chunk[0] = '\0';
    SQLLEN chunk_ind = 0;
    SQLRETURN rc = SQLGetData(
      stmt.native(), column, SQL_C_CHAR, chunk, sizeof(chunk), &chunk_ind);
    if (rc == SQL_NO_DATA || chunk_ind == SQL_NULL_DATA) {
      return out;
    }
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt.native(), "read long character data");
    out.append(chunk, std::strlen(chunk));
    if (rc != SQL_SUCCESS_WITH_INFO) {
      return out;
    }
  }
}

std::vector<std::byte> get_data_binary(
  odbc::statement& stmt, SQLUSMALLINT column) {
  std::vector<std::byte> out;
  for (;;) {
    std::byte chunk[4096];
    SQLLEN chunk_ind = 0;
    SQLRETURN rc = SQLGetData(
      stmt.native(), column, SQL_C_BINARY, chunk, sizeof(chunk), &chunk_ind);
    if (rc == SQL_NO_DATA || chunk_ind == SQL_NULL_DATA) {
      return out;
    }
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt.native(), "read long binary data");
    std::size_t take;
    if (rc == SQL_SUCCESS_WITH_INFO || chunk_ind == SQL_NO_TOTAL) {
      take = sizeof(chunk);
    } else {
      take = std::min<SQLLEN>(chunk_ind, static_cast<SQLLEN>(sizeof(chunk)));
    }
    out.insert(out.end(), chunk, chunk + take);
    if (rc != SQL_SUCCESS_WITH_INFO) {
      return out;
    }
  }
}

SQLSMALLINT sql_type_for(backend::buffer_type t) {
  switch (t) {
  case backend::buffer_type::bit:
    return SQL_BIT;
  case backend::buffer_type::int8:
    return SQL_TINYINT;
  case backend::buffer_type::int16:
    return SQL_SMALLINT;
  case backend::buffer_type::int32:
    return SQL_INTEGER;
  case backend::buffer_type::int64:
    return SQL_BIGINT;
  case backend::buffer_type::float32:
    return SQL_FLOAT;
  case backend::buffer_type::float64:
    return SQL_DOUBLE;
  case backend::buffer_type::chars:
    return SQL_VARCHAR;
  case backend::buffer_type::bytes:
    return SQL_VARBINARY;
  case backend::buffer_type::timestamp_parts:
    return SQL_TYPE_TIMESTAMP;
  case backend::buffer_type::date_parts:
    return SQL_TYPE_DATE;
  case backend::buffer_type::time_parts:
    return SQL_TYPE_TIME;
  }
  throw backend::backend_error("odbc", "unsupported buffer type", {});
}

backend::buffer_type buffer_type_for(sql_value const& v) {
  if (std::holds_alternative<std::monostate>(v))
    return backend::buffer_type::chars;
  if (std::holds_alternative<bool>(v))
    return backend::buffer_type::bit;
  if (std::holds_alternative<std::int16_t>(v))
    return backend::buffer_type::int16;
  if (std::holds_alternative<std::int32_t>(v))
    return backend::buffer_type::int32;
  if (std::holds_alternative<std::int64_t>(v))
    return backend::buffer_type::int64;
  if (std::holds_alternative<double>(v))
    return backend::buffer_type::float64;
  if (std::holds_alternative<std::string>(v))
    return backend::buffer_type::chars;
  if (std::holds_alternative<std::vector<std::byte>>(v))
    return backend::buffer_type::bytes;
  if (std::holds_alternative<timestamp>(v))
    return backend::buffer_type::timestamp_parts;
  return backend::buffer_type::chars;
}

}  // namespace

struct backend_statement::param_slot {
  SQLLEN indicator = SQL_NULL_DATA;
  unsigned char bit = 0;
  SQL_TIMESTAMP_STRUCT ts{};
};

backend_statement::backend_statement(odbc::connection& conn) : stmt_(conn) {}

void backend_statement::prepare(std::string_view sql) {
  stmt_.prepare(sql);
}

void backend_statement::bind_parameter(
  std::size_t index, sql_value const& value) {
  if (slots_.size() < index) {
    slots_.resize(index);
  }
  param_slot& slot = slots_[index - 1];
  auto odbc_index = static_cast<SQLUSMALLINT>(index);

  if (std::holds_alternative<std::monostate>(value)) {
    slot.indicator = SQL_NULL_DATA;
    stmt_.bind_parameter(
      odbc_index, SQL_C_CHAR, SQL_VARCHAR, nullptr, 0, &slot.indicator, 1);
  } else if (auto* p = std::get_if<bool>(&value)) {
    slot.bit = *p ? 1 : 0;
    slot.indicator = sizeof(slot.bit);
    stmt_.bind_parameter(odbc_index, SQL_C_BIT, SQL_BIT, &slot.bit,
      sizeof(slot.bit), &slot.indicator);
  } else if (auto* p = std::get_if<std::int16_t>(&value)) {
    slot.indicator = sizeof(*p);
    stmt_.bind_parameter(odbc_index, SQL_C_SSHORT, SQL_SMALLINT,
      const_cast<std::int16_t*>(p), sizeof(*p), &slot.indicator);
  } else if (auto* p = std::get_if<std::int32_t>(&value)) {
    slot.indicator = sizeof(*p);
    stmt_.bind_parameter(odbc_index, SQL_C_SLONG, SQL_INTEGER,
      const_cast<std::int32_t*>(p), sizeof(*p), &slot.indicator);
  } else if (auto* p = std::get_if<std::int64_t>(&value)) {
    slot.indicator = sizeof(*p);
    stmt_.bind_parameter(odbc_index, SQL_C_SBIGINT, SQL_BIGINT,
      const_cast<std::int64_t*>(p), sizeof(*p), &slot.indicator);
  } else if (auto* p = std::get_if<double>(&value)) {
    slot.indicator = sizeof(*p);
    stmt_.bind_parameter(odbc_index, SQL_C_DOUBLE, SQL_DOUBLE,
      const_cast<double*>(p), sizeof(*p), &slot.indicator);
  } else if (auto* p = std::get_if<std::string>(&value)) {
    slot.indicator = static_cast<SQLLEN>(p->size());
    stmt_.bind_parameter(odbc_index, SQL_C_CHAR, SQL_VARCHAR,
      const_cast<char*>(p->data()), static_cast<SQLLEN>(p->size()),
      &slot.indicator, std::max<SQLULEN>(p->size(), 1));
  } else if (auto* p = std::get_if<std::vector<std::byte>>(&value)) {
    slot.indicator = static_cast<SQLLEN>(p->size());
    stmt_.bind_parameter(odbc_index, SQL_C_BINARY, SQL_VARBINARY,
      const_cast<std::byte*>(p->data()), static_cast<SQLLEN>(p->size()),
      &slot.indicator, std::max<SQLULEN>(p->size(), 1));
  } else if (auto* p = std::get_if<timestamp>(&value)) {
    auto parts = uniorm::detail::break_timestamp(*p);
    slot.ts.year = static_cast<SQLSMALLINT>(parts.year);
    slot.ts.month = static_cast<SQLUSMALLINT>(parts.month);
    slot.ts.day = static_cast<SQLUSMALLINT>(parts.day);
    slot.ts.hour = static_cast<SQLUSMALLINT>(parts.hour);
    slot.ts.minute = static_cast<SQLUSMALLINT>(parts.minute);
    slot.ts.second = static_cast<SQLUSMALLINT>(parts.second);
    slot.ts.fraction = static_cast<SQLUINTEGER>(parts.fraction_ns);
    slot.indicator = sizeof(slot.ts);
    stmt_.bind_parameter(odbc_index, SQL_C_TYPE_TIMESTAMP,
      SQL_TYPE_TIMESTAMP, &slot.ts, sizeof(slot.ts), &slot.indicator, 19, 0);
  }
}

void backend_statement::bind_column(
  std::size_t index, backend::column_buffer const& buffer) {
  stmt_.bind_column(static_cast<SQLUSMALLINT>(index), c_type_for(buffer.type),
    buffer.data, static_cast<SQLLEN>(buffer.capacity),
    reinterpret_cast<SQLLEN*>(buffer.indicator));
}

void backend_statement::bind_batch_params(std::vector<params> const& rows) {
  if (rows.empty()) {
    return;
  }
  std::size_t const n = rows.size();
  std::size_t const num_cols = rows[0].size();

  batch_cols_.clear();
  batch_cols_.resize(num_cols);

  for (std::size_t c = 0; c < num_cols; ++c) {
    auto& col = batch_cols_[c];
    col.count = n;

    backend::buffer_type btype = backend::buffer_type::chars;
    for (std::size_t r = 0; r < n; ++r) {
      auto const& v = rows[r].at(c);
      if (!std::holds_alternative<std::monostate>(v)) {
        btype = buffer_type_for(v);
        break;
      }
    }
    col.type = btype;
    col.indicators.resize(n, backend::null_indicator);

    switch (btype) {
    case backend::buffer_type::bit:
      col.bit_vals.resize(n, 0);
      col.stride = sizeof(unsigned char);
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<bool>(&rows[r].at(c))) {
          col.bit_vals[r] = *p ? 1 : 0;
          col.indicators[r] = sizeof(unsigned char);
        }
      }
      break;
    case backend::buffer_type::int16:
      col.i16_vals.resize(n, 0);
      col.stride = sizeof(std::int16_t);
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<std::int16_t>(&rows[r].at(c))) {
          col.i16_vals[r] = *p;
          col.indicators[r] = sizeof(std::int16_t);
        }
      }
      break;
    case backend::buffer_type::int32:
      col.i32_vals.resize(n, 0);
      col.stride = sizeof(std::int32_t);
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<std::int32_t>(&rows[r].at(c))) {
          col.i32_vals[r] = *p;
          col.indicators[r] = sizeof(std::int32_t);
        }
      }
      break;
    case backend::buffer_type::int64:
      col.i64_vals.resize(n, 0);
      col.stride = sizeof(std::int64_t);
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<std::int64_t>(&rows[r].at(c))) {
          col.i64_vals[r] = *p;
          col.indicators[r] = sizeof(std::int64_t);
        }
      }
      break;
    case backend::buffer_type::float64:
      col.f64_vals.resize(n, 0);
      col.stride = sizeof(double);
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<double>(&rows[r].at(c))) {
          col.f64_vals[r] = *p;
          col.indicators[r] = sizeof(double);
        }
      }
      break;
    case backend::buffer_type::chars: {
      std::size_t max_len = 1;
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<std::string>(&rows[r].at(c))) {
          max_len = std::max(max_len, p->size() + 1);
        }
      }
      col.stride = max_len;
      col.var_buf.resize(n * max_len, '\0');
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<std::string>(&rows[r].at(c))) {
          std::memcpy(&col.var_buf[r * max_len], p->data(), p->size());
          col.indicators[r] = static_cast<SQLLEN>(p->size());
        }
      }
      break;
    }
    case backend::buffer_type::bytes: {
      std::size_t max_len = 1;
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<std::vector<std::byte>>(&rows[r].at(c))) {
          max_len = std::max(max_len, p->size());
        }
      }
      col.stride = max_len;
      col.var_buf.resize(n * max_len, '\0');
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<std::vector<std::byte>>(&rows[r].at(c))) {
          std::memcpy(&col.var_buf[r * max_len], p->data(), p->size());
          col.indicators[r] = static_cast<SQLLEN>(p->size());
        }
      }
      break;
    }
    case backend::buffer_type::timestamp_parts:
      col.ts_vals.resize(n);
      col.stride = sizeof(backend::timestamp_parts);
      for (std::size_t r = 0; r < n; ++r) {
        if (auto* p = std::get_if<timestamp>(&rows[r].at(c))) {
          auto parts = uniorm::detail::break_timestamp(*p);
          auto& ts = col.ts_vals[r];
          ts.year = static_cast<std::int16_t>(parts.year);
          ts.month = static_cast<std::uint16_t>(parts.month);
          ts.day = static_cast<std::uint16_t>(parts.day);
          ts.hour = static_cast<std::uint16_t>(parts.hour);
          ts.minute = static_cast<std::uint16_t>(parts.minute);
          ts.second = static_cast<std::uint16_t>(parts.second);
          ts.fraction_ns = parts.fraction_ns;
          col.indicators[r] = sizeof(backend::timestamp_parts);
        }
      }
      break;
    default:
      col.stride = 1;
      break;
    }
  }

  for (std::size_t c = 0; c < num_cols; ++c) {
    auto& col = batch_cols_[c];
    auto odbc_index = static_cast<SQLUSMALLINT>(c + 1);
    SQLSMALLINT c_type = c_type_for(col.type);
    SQLSMALLINT sql_type = sql_type_for(col.type);
    SQLULEN column_size = 0;
    SQLSMALLINT decimal_digits = 0;
    if (col.type == backend::buffer_type::timestamp_parts) {
      column_size = 19;
      decimal_digits = 0;
    } else if (col.type == backend::buffer_type::chars ||
               col.type == backend::buffer_type::bytes) {
      column_size = std::max(static_cast<SQLULEN>(col.stride), SQLULEN(255));
    }

    SQLLEN* ind_ptr = reinterpret_cast<SQLLEN*>(col.indicators.data());

    void* data_ptr = nullptr;
    switch (col.type) {
    case backend::buffer_type::bit:
      data_ptr = col.bit_vals.data();
      break;
    case backend::buffer_type::int16:
      data_ptr = col.i16_vals.data();
      break;
    case backend::buffer_type::int32:
      data_ptr = col.i32_vals.data();
      break;
    case backend::buffer_type::int64:
      data_ptr = col.i64_vals.data();
      break;
    case backend::buffer_type::float64:
      data_ptr = col.f64_vals.data();
      break;
    case backend::buffer_type::timestamp_parts:
      data_ptr = col.ts_vals.data();
      break;
    default:
      data_ptr = col.var_buf.data();
      break;
    }

    SQLRETURN rc = SQLBindParameter(stmt_.native(), odbc_index, SQL_PARAM_INPUT,
      c_type, sql_type, column_size, decimal_digits, data_ptr,
      static_cast<SQLLEN>(col.stride), ind_ptr);
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt_.native(), "bind parameter array");
  }
}

class backend_statement::odbc_batch_writer
  : public backend::batch_writer_iface {
public:
  explicit odbc_batch_writer(backend_statement& stmt) : stmt_(stmt) {}

  std::size_t add_column(backend::buffer_type type, std::size_t count,
    std::size_t element_size) override {
    auto idx = stmt_.batch_cols_.size();
    stmt_.batch_cols_.emplace_back();
    auto& col = stmt_.batch_cols_.back();
    col.type = type;
    col.count = count;
    col.stride = element_size;
    col.indicators.resize(count, backend::null_indicator);

    switch (type) {
    case backend::buffer_type::bit:
      col.bit_vals.resize(count, 0);
      break;
    case backend::buffer_type::int16:
      col.i16_vals.resize(count, 0);
      break;
    case backend::buffer_type::int32:
      col.i32_vals.resize(count, 0);
      break;
    case backend::buffer_type::int64:
      col.i64_vals.resize(count, 0);
      break;
    case backend::buffer_type::float64:
      col.f64_vals.resize(count, 0);
      break;
    case backend::buffer_type::chars:
    case backend::buffer_type::bytes:
      col.var_buf.resize(count * element_size, '\0');
      break;
    case backend::buffer_type::timestamp_parts:
      col.ts_vals.resize(count);
      break;
    default:
      break;
    }
    return idx;
  }

  void* data(std::size_t column) override {
    auto& col = stmt_.batch_cols_[column];
    switch (col.type) {
    case backend::buffer_type::bit:
      return col.bit_vals.data();
    case backend::buffer_type::int16:
      return col.i16_vals.data();
    case backend::buffer_type::int32:
      return col.i32_vals.data();
    case backend::buffer_type::int64:
      return col.i64_vals.data();
    case backend::buffer_type::float64:
      return col.f64_vals.data();
    case backend::buffer_type::timestamp_parts:
      return col.ts_vals.data();
    default:
      return col.var_buf.data();
    }
  }

  std::size_t element_size(std::size_t column) override {
    return stmt_.batch_cols_[column].stride;
  }

  std::int64_t* indicators(std::size_t column) override {
    return stmt_.batch_cols_[column].indicators.data();
  }

  void finish() override {
    SQLFreeStmt(stmt_.stmt_.native(), SQL_RESET_PARAMS);
    for (std::size_t c = 0; c < stmt_.batch_cols_.size(); ++c) {
      auto& col = stmt_.batch_cols_[c];
      auto odbc_index = static_cast<SQLUSMALLINT>(c + 1);
      SQLSMALLINT c_type = c_type_for(col.type);
      SQLSMALLINT sql_type = sql_type_for(col.type);
      auto stride = static_cast<SQLLEN>(col.stride);
      SQLULEN column_size = 0;
      SQLSMALLINT decimal_digits = 0;
      if (col.type == backend::buffer_type::timestamp_parts) {
        column_size = 19;
        decimal_digits = 0;
      } else if (col.type == backend::buffer_type::chars ||
                 col.type == backend::buffer_type::bytes) {
        column_size = std::max(static_cast<SQLULEN>(col.stride), SQLULEN(255));
      }

      SQLLEN* ind_ptr = reinterpret_cast<SQLLEN*>(col.indicators.data());

      void* data_ptr = data(c);
      SQLRETURN rc = SQLBindParameter(stmt_.stmt_.native(), odbc_index,
        SQL_PARAM_INPUT, c_type, sql_type, column_size, decimal_digits,
        data_ptr, stride, ind_ptr);
      odbc::throw_if_error(
        rc, SQL_HANDLE_STMT, stmt_.stmt_.native(), "bind parameter array");
    }
  }

private:
  backend_statement& stmt_;
};

backend::batch_writer_iface& backend_statement::prepare_batch() {
  batch_cols_.clear();
  if (!batch_writer_) {
    batch_writer_ = std::make_unique<odbc_batch_writer>(*this);
  }
  return *batch_writer_;
}

void backend_statement::execute() {
  stmt_.execute();
}

bool backend_statement::fetch() {
  return stmt_.fetch();
}

void backend_statement::set_row_array_size(std::size_t size) {
  stmt_.set_row_array_size(static_cast<SQLULEN>(size));
}

std::size_t backend_statement::rows_fetched() const {
  return static_cast<std::size_t>(stmt_.rows_fetched());
}

void backend_statement::set_paramset_size(std::size_t size) {
  stmt_.set_paramset_size(static_cast<SQLULEN>(size));
}

std::size_t backend_statement::affected_rows() const {
  return stmt_.affected_rows();
}

std::size_t backend_statement::result_row_estimate() const {
  // SQLRowCount reports the full row count for buffered result sets
  // (MariaDB/MySQL use buffered cursors by default). Drivers without
  // this capability return -1, which affected_rows maps to 0.
  return stmt_.affected_rows();
}

std::vector<column_info> backend_statement::column_meta() const {
  SQLSMALLINT count = static_cast<SQLSMALLINT>(stmt_.column_count());
  std::vector<column_info> meta;
  meta.resize(static_cast<std::size_t>(count));
  for (SQLSMALLINT i = 0; i < count; ++i) {
    SQLCHAR name_buf[512];
    SQLSMALLINT name_len = 0;
    SQLSMALLINT odbc_type = 0;
    SQLULEN display_size = 0;
    SQLSMALLINT decimals = 0;
    SQLSMALLINT nullable = 0;
    SQLRETURN rc = SQLDescribeCol(stmt_.native(),
      static_cast<SQLUSMALLINT>(i + 1), name_buf,
      static_cast<SQLSMALLINT>(sizeof(name_buf)), &name_len, &odbc_type,
      &display_size, &decimals, &nullable);
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt_.native(), "describe result column");

    column_info& info = meta[static_cast<std::size_t>(i)];
    info.name.assign(reinterpret_cast<char const*>(name_buf), name_len);
    info.type = sql_type_from_native(odbc_type);
    info.display_size = display_size;
    info.scale = decimals > 0 ? static_cast<std::size_t>(decimals) : 0;
    info.nullable = nullable != SQL_NO_NULLS;
  }
  return meta;
}

std::string backend_statement::read_long_text(std::size_t column) {
  return get_data_char(stmt_, static_cast<SQLUSMALLINT>(column));
}

std::vector<std::byte> backend_statement::read_long_bytes(
  std::size_t column) {
  return get_data_binary(stmt_, static_cast<SQLUSMALLINT>(column));
}

void backend_statement::reset() {
  slots_.clear();
  batch_cols_.clear();
  batch_cols_.shrink_to_fit();
  batch_writer_.reset();
  stmt_.reset();
}

struct backend_connection::schema_metadata_impl
  : backend::schema_metadata {
  explicit schema_metadata_impl(odbc::connection& conn) : conn_(conn) {}

  std::vector<column_row> table_columns(std::string_view table) override {
    std::string table_str(table);
    odbc::statement stmt(conn_);
    SQLRETURN rc = SQLColumns(stmt.native(), nullptr, 0, nullptr, 0,
      reinterpret_cast<SQLCHAR*>(table_str.data()), SQL_NTS, nullptr, 0);
    odbc::throw_if_error(
      rc, SQL_HANDLE_STMT, stmt.native(), "SQLColumns(" + table_str + ")");

    struct buffers {
      char name[256] = {};
      SQLLEN name_ind = SQL_NULL_DATA;
      SQLINTEGER data_type = 0;
      SQLLEN type_ind = SQL_NULL_DATA;
      SQLSMALLINT nullable = SQL_NO_NULLS;
      SQLLEN null_ind = SQL_NULL_DATA;
    } buf;

    stmt.bind_column(4, SQL_C_CHAR, buf.name, sizeof(buf.name),
      &buf.name_ind);
    stmt.bind_column(
      5, SQL_C_SLONG, &buf.data_type, sizeof(buf.data_type), &buf.type_ind);
    stmt.bind_column(
      11, SQL_C_SSHORT, &buf.nullable, sizeof(buf.nullable), &buf.null_ind);

    std::vector<column_row> rows;
    while (stmt.fetch()) {
      if (buf.name_ind == SQL_NULL_DATA) {
        continue;
      }
      column_row row;
      row.name.assign(buf.name, static_cast<std::size_t>(buf.name_ind));
      row.type = sql_type_from_native(buf.data_type);
      row.native_type = buf.data_type;
      row.nullable = buf.nullable != SQL_NO_NULLS;
      rows.push_back(std::move(row));
    }
    return rows;
  }

  odbc::connection& conn_;
};

backend_connection::backend_connection()
  : conn_(shared_environment()),
    metadata_(std::make_unique<schema_metadata_impl>(conn_)) {}

void backend_connection::open(std::string_view connection_string) {
  conn_.open(connection_string);
}

void backend_connection::close() {
  conn_.close();
}

bool backend_connection::is_open() const noexcept {
  return conn_.is_open();
}

void backend_connection::set_autocommit(bool enabled) {
  conn_.set_autocommit(enabled);
}

void backend_connection::commit() {
  conn_.commit();
}

void backend_connection::rollback() {
  conn_.rollback();
}

backend::capabilities backend_connection::caps() const noexcept {
  return {/*streaming=*/true, /*async_io=*/false, /*copy_protocol=*/false,
    /*notifications=*/false, /*columnar_batch=*/true};
}

std::string backend_connection::dbms_name() const {
  char buffer[128] = {};
  SQLSMALLINT length = 0;
  SQLRETURN rc =
    SQLGetInfo(conn_.native(), SQL_DBMS_NAME, buffer, sizeof(buffer),
      &length);
  odbc::throw_if_error(
    rc, SQL_HANDLE_DBC, conn_.native(), "SQLGetInfo(SQL_DBMS_NAME)");
  return std::string(buffer, static_cast<std::size_t>(length));
}

std::unique_ptr<backend::statement_iface>
backend_connection::create_statement() {
  return std::make_unique<backend_statement>(conn_);
}

void* backend_connection::native_handle() noexcept {
  return conn_.native();
}

void* backend_connection::extension(std::type_index id) noexcept {
  if (id == std::type_index(typeid(backend::schema_metadata))) {
    return static_cast<backend::schema_metadata*>(metadata_.get());
  }
  return nullptr;
}

namespace {

backend::registrar const registered(
  "odbc", [] { return std::make_unique<backend_connection>(); });

}  // namespace

}  // namespace uniorm::odbc
