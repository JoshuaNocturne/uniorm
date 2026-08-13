#include "uniorm/result_set.hpp"

#include <cstring>
#include <utility>
#include <vector>

#include <sql.h>
#include <sqlext.h>

#include "uniorm/detail/time.hpp"
#include "uniorm/odbc/error.hpp"
#include "uniorm/odbc/statement.hpp"

namespace uniorm {

namespace {

enum class slot_kind { boolean, integer, floating, text, bytes, ts, dt, tm };

struct column_slot {
  column_info info;
  slot_kind kind = slot_kind::text;

  unsigned char bit_val = 0;
  std::int64_t int_val = 0;
  double dbl_val = 0.0;
  std::vector<char> text_buf;
  std::vector<std::byte> bin_buf;
  SQL_TIMESTAMP_STRUCT ts_val{};
  SQL_DATE_STRUCT date_val{};
  SQL_TIME_STRUCT time_val{};
  SQLLEN indicator = 0;
};

slot_kind kind_for(SQLSMALLINT odbc_type) {
  switch (odbc_type) {
  case SQL_BIT:
    return slot_kind::boolean;
  case SQL_TINYINT:
  case SQL_SMALLINT:
  case SQL_INTEGER:
  case SQL_BIGINT:
    return slot_kind::integer;
  case SQL_REAL:
  case SQL_FLOAT:
  case SQL_DOUBLE:
  case SQL_DECIMAL:
  case SQL_NUMERIC:
    return slot_kind::floating;
  case SQL_BINARY:
  case SQL_VARBINARY:
  case SQL_LONGVARBINARY:
    return slot_kind::bytes;
  case SQL_TYPE_TIMESTAMP:
  case SQL_TIMESTAMP:
    return slot_kind::ts;
  case SQL_TYPE_DATE:
  case SQL_DATE:
    return slot_kind::dt;
  case SQL_TYPE_TIME:
  case SQL_TIME:
    return slot_kind::tm;
  default:
    return slot_kind::text;  // includes all char/wchar/guid variants
  }
}

// Some drivers (e.g. MariaDB Connector/ODBC) return the FULL value from
// SQLGetData after a truncated bound-column fetch, not just the tail. Read
// into a fresh container so callers replace the partial bound buffer.
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
    // SQL_SUCCESS_WITH_INFO (01004) means the chunk was truncated; keep going.
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
      take = sizeof(chunk);  // truncated: buffer is full
    } else {
      take = std::min<SQLLEN>(chunk_ind, static_cast<SQLLEN>(sizeof(chunk)));
    }
    out.insert(out.end(), chunk, chunk + take);
    if (rc != SQL_SUCCESS_WITH_INFO) {
      return out;
    }
  }
}

}  // namespace

struct result_set::impl {
  odbc::statement stmt;
  std::vector<column_slot> slots;
  std::shared_ptr<column_names> names;

  explicit impl(odbc::statement s) : stmt(std::move(s)) {
    describe_and_bind();
  }

  void describe_and_bind() {
    SQLSMALLINT count = static_cast<SQLSMALLINT>(stmt.column_count());
    slots.resize(static_cast<std::size_t>(count));
    std::vector<std::string> collected;
    collected.reserve(count);

    for (SQLSMALLINT i = 0; i < count; ++i) {
      SQLCHAR name_buf[512];
      SQLSMALLINT name_len = 0;
      SQLSMALLINT odbc_type = 0;
      SQLULEN display_size = 0;
      SQLSMALLINT decimals = 0;
      SQLSMALLINT nullable = 0;
      SQLRETURN rc =
        SQLDescribeCol(stmt.native(), static_cast<SQLUSMALLINT>(i + 1),
          name_buf, static_cast<SQLSMALLINT>(sizeof(name_buf)), &name_len,
          &odbc_type, &display_size, &decimals, &nullable);
      odbc::throw_if_error(
        rc, SQL_HANDLE_STMT, stmt.native(), "describe result column");

      column_slot& s = slots[static_cast<std::size_t>(i)];
      s.info.name.assign(reinterpret_cast<char const*>(name_buf), name_len);
      s.info.type = sql_type_from_native(odbc_type);
      s.info.display_size = display_size;
      s.info.nullable = nullable != SQL_NO_NULLS;
      s.kind = kind_for(odbc_type);

      SQLSMALLINT c_type;
      SQLPOINTER buffer;
      SQLLEN buffer_length;
      switch (s.kind) {
      case slot_kind::boolean:
        c_type = SQL_C_BIT;
        buffer = &s.bit_val;
        buffer_length = sizeof(s.bit_val);
        break;
      case slot_kind::integer:
        c_type = SQL_C_SBIGINT;
        buffer = &s.int_val;
        buffer_length = sizeof(s.int_val);
        break;
      case slot_kind::floating:
        c_type = SQL_C_DOUBLE;
        buffer = &s.dbl_val;
        buffer_length = sizeof(s.dbl_val);
        break;
      case slot_kind::bytes: {
        std::size_t capacity = std::max<std::size_t>(display_size, 32);
        s.bin_buf.resize(capacity);
        c_type = SQL_C_BINARY;
        buffer = s.bin_buf.data();
        buffer_length = static_cast<SQLLEN>(capacity);
        break;
      }
      case slot_kind::ts:
        c_type = SQL_C_TYPE_TIMESTAMP;
        buffer = &s.ts_val;
        buffer_length = sizeof(s.ts_val);
        break;
      case slot_kind::dt:
        c_type = SQL_C_TYPE_DATE;
        buffer = &s.date_val;
        buffer_length = sizeof(s.date_val);
        break;
      case slot_kind::tm:
        c_type = SQL_C_TYPE_TIME;
        buffer = &s.time_val;
        buffer_length = sizeof(s.time_val);
        break;
      case slot_kind::text:
      default: {
        std::size_t capacity = std::max<std::size_t>(display_size, 31) + 1;
        s.text_buf.resize(capacity);
        c_type = SQL_C_CHAR;
        buffer = s.text_buf.data();
        buffer_length = static_cast<SQLLEN>(capacity);
        break;
      }
      }

      SQLRETURN brc =
        SQLBindCol(stmt.native(), static_cast<SQLUSMALLINT>(i + 1), c_type,
          buffer, buffer_length, &s.indicator);
      odbc::throw_if_error(
        brc, SQL_HANDLE_STMT, stmt.native(), "bind result column");
      collected.push_back(s.info.name);
    }

    names = std::make_shared<column_names>(std::move(collected));
  }

  sql_value value_of(std::size_t i) {
    column_slot& s = slots[i];
    if (s.indicator == SQL_NULL_DATA) {
      return std::monostate{};
    }
    switch (s.kind) {
    case slot_kind::boolean:
      return s.bit_val != 0;
    case slot_kind::integer:
      return s.int_val;
    case slot_kind::floating:
      return s.dbl_val;
    case slot_kind::text: {
      if (s.indicator == SQL_NO_TOTAL ||
          s.indicator > static_cast<SQLLEN>(s.text_buf.size()) - 1) {
        return get_data_char(stmt, static_cast<SQLUSMALLINT>(i + 1));
      }
      return std::string(
        s.text_buf.data(), static_cast<std::size_t>(s.indicator));
    }
    case slot_kind::bytes: {
      bool truncated = s.indicator == SQL_NO_TOTAL ||
                       s.indicator > static_cast<SQLLEN>(s.bin_buf.size());
      if (truncated) {
        return get_data_binary(stmt, static_cast<SQLUSMALLINT>(i + 1));
      }
      return std::vector<std::byte>(s.bin_buf.begin(),
        s.bin_buf.begin() + static_cast<std::ptrdiff_t>(s.indicator));
    }
    case slot_kind::ts:
      return detail::make_timestamp(s.ts_val.year, s.ts_val.month, s.ts_val.day,
        s.ts_val.hour, s.ts_val.minute, s.ts_val.second, s.ts_val.fraction);
    case slot_kind::dt:
      return detail::make_timestamp(
        s.date_val.year, s.date_val.month, s.date_val.day, 0, 0, 0, 0);
    case slot_kind::tm:
      return detail::make_timestamp(
        1970, 1, 1, s.time_val.hour, s.time_val.minute, s.time_val.second, 0);
    }
    return std::monostate{};
  }
};

result_set::result_set(std::unique_ptr<impl> i) : impl_(std::move(i)) {}

result_set::~result_set() = default;

result_set::result_set(result_set&&) noexcept = default;

result_set& result_set::operator=(result_set&&) noexcept = default;

result_set result_set::from_statement(odbc::statement stmt) {
  return result_set(std::make_unique<impl>(std::move(stmt)));
}

bool result_set::next() {
  return impl_->stmt.fetch();
}

row result_set::current() {
  std::vector<sql_value> values;
  values.reserve(impl_->slots.size());
  for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
    values.push_back(impl_->value_of(i));
  }
  return row(impl_->names, std::move(values));
}

std::size_t result_set::column_count() const {
  return impl_->slots.size();
}

column_info const& result_set::column(std::size_t index) const {
  return impl_->slots.at(index).info;
}

}  // namespace uniorm
