#include <uniorm/detail/connection.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <variant>

#include "uniorm/backend/registry.hpp"
#include "uniorm/detail/time.hpp"
#include "uniorm/dialect.hpp"
#include "uniorm/query/builder.hpp"
#include "uniorm/transaction.hpp"

namespace uniorm {

connection::connection(std::string_view connection_string)
  : stmt_cache_(std::make_shared<detail::statement_cache>()) {
  std::string tail;
  backend_ = backend::registry::instance().create(connection_string, &tail);
  backend_->open(tail);
}

connection::~connection() = default;

connection::connection(connection&&) noexcept = default;

connection& connection::operator=(connection&&) noexcept = default;

void connection::close() {
  clear_statement_cache();
  backend_->close();
}

bool connection::is_open() const noexcept {
  return backend_ && backend_->is_open();
}

result_set connection::execute(std::string_view sql, params const& p) {
  std::string key(sql);
  auto stmt = acquire_cached(key);
  bind_parameters(*stmt, p);
  stmt->execute();
  return result_set::from_statement(
    std::move(stmt), make_releaser(key), row_array_size_);
}

std::size_t connection::execute_update(std::string_view sql, params const& p) {
  std::string key(sql);
  auto stmt = acquire_cached(key);
  bind_parameters(*stmt, p);
  stmt->execute();
  std::size_t affected = stmt->affected_rows();
  stmt_cache_->release(key, std::move(stmt));
  return affected;
}

namespace {

bool blank(std::string_view s) {
  return std::all_of(
    s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

}  // namespace

update_builder::update_builder(connection& conn, std::string table)
  : conn_(&conn), table_(std::move(table)) {}

update_builder& update_builder::where(std::string_view clause, params p) {
  where_ = std::string(clause);
  where_params_ = std::move(p);
  return *this;
}

std::size_t update_builder::execute() {
  if (set_.empty()) {
    throw uniorm_error("update: no columns to set");
  }
  if (blank(where_)) {
    throw uniorm_error("update: refusing to execute without a WHERE clause");
  }
  dialect const d = dialect::detect(conn_->dbms_name());
  std::string sql = "UPDATE " + d.quote_identifier(table_) + " SET ";
  std::vector<sql_value> values;
  values.reserve(set_.size() + where_params_.size());
  for (std::size_t i = 0; i < set_.size(); ++i) {
    if (i != 0) {
      sql += ", ";
    }
    sql += d.quote_identifier(set_[i].first) + " = ?";
    values.push_back(set_[i].second);
  }
  sql += " WHERE " + where_;
  auto const& wp = where_params_.values();
  values.insert(values.end(), wp.begin(), wp.end());
  return conn_->execute_update(sql, params(std::move(values)));
}

remove_builder::remove_builder(connection& conn, std::string table)
  : conn_(&conn), table_(std::move(table)) {}

remove_builder& remove_builder::where(std::string_view clause, params p) {
  where_ = std::string(clause);
  where_params_ = std::move(p);
  return *this;
}

std::size_t remove_builder::execute() {
  if (blank(where_)) {
    throw uniorm_error("remove: refusing to execute without a WHERE clause");
  }
  dialect const d = dialect::detect(conn_->dbms_name());
  std::string sql =
    "DELETE FROM " + d.quote_identifier(table_) + " WHERE " + where_;
  return conn_->execute_update(sql, where_params_);
}

update_builder connection::update(std::string_view table) {
  return update_builder(*this, std::string(table));
}

remove_builder connection::remove(std::string_view table) {
  return remove_builder(*this, std::string(table));
}

std::function<void(std::unique_ptr<backend::statement_iface>)>
connection::make_releaser(std::string key) {
  std::weak_ptr<detail::statement_cache> weak = stmt_cache_;
  return [weak, key = std::move(key)](
           std::unique_ptr<backend::statement_iface> stmt) noexcept {
    if (auto cache = weak.lock()) {
      cache->release(key, std::move(stmt));
    }
  };
}

unsigned long long connection::statement_cache_hits() const {
  return stmt_cache_->hits;
}

unsigned long long connection::statement_cache_misses() const {
  return stmt_cache_->misses;
}

std::size_t connection::statement_cache_size() const {
  return stmt_cache_->entries.size();
}

void connection::clear_statement_cache() {
  stmt_cache_->entries.clear();
  stmt_cache_->lru.clear();
}

namespace {

// Keeps batch inserts within driver paramset limits.
constexpr std::size_t default_paramset_batch = 1000;

// Column-wise parameter array storage for batch inserts.
// Each column gets a contiguous buffer of N values plus N indicators.
struct batch_column {
  backend::buffer_type type = backend::buffer_type::chars;

  // Fixed-size type arrays (only the relevant one is populated)
  std::vector<unsigned char> bit_vals;
  std::vector<std::int16_t> i16_vals;
  std::vector<std::int32_t> i32_vals;
  std::vector<std::int64_t> i64_vals;
  std::vector<double> f64_vals;
  std::vector<backend::timestamp_parts> ts_vals;

  // Variable-size type buffer (chars or bytes), stride = max_elem_size
  std::vector<char> var_buf;
  std::size_t stride = 0;

  std::vector<std::int64_t> indicators;
  std::size_t count = 0;
};

// Determine the buffer_type for a sql_value.
backend::buffer_type buffer_type_for(sql_value const& v) {
  if (std::holds_alternative<std::monostate>(v))
    return backend::buffer_type::chars;  // null defaults to chars
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

void build_batch_columns(std::vector<batch_column>& cols,
  std::vector<params> const& rows, std::size_t start, std::size_t end,
  std::size_t num_columns) {
  std::size_t n = end - start;
  cols.resize(num_columns);

  for (std::size_t c = 0; c < num_columns; ++c) {
    batch_column& col = cols[c];
    col.count = n;
    col.indicators.resize(n);

    // Determine type from first non-null value in this column
    backend::buffer_type btype = backend::buffer_type::chars;
    for (std::size_t r = 0; r < n; ++r) {
      auto const& v = rows[start + r].at(c);
      if (!std::holds_alternative<std::monostate>(v)) {
        btype = buffer_type_for(v);
        break;
      }
    }
    col.type = btype;

    switch (btype) {
    case backend::buffer_type::bit:
      col.bit_vals.resize(n, 0);
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<bool>(&v)) {
          col.bit_vals[r] = *p ? 1 : 0;
          col.indicators[r] = sizeof(unsigned char);
        } else if (std::holds_alternative<std::monostate>(v)) {
          col.indicators[r] = backend::null_indicator;
        } else {
          col.bit_vals[r] = 0;
          col.indicators[r] = backend::null_indicator;
        }
      }
      break;
    case backend::buffer_type::int16:
      col.i16_vals.resize(n, 0);
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<std::int16_t>(&v)) {
          col.i16_vals[r] = *p;
          col.indicators[r] = sizeof(std::int16_t);
        } else if (std::holds_alternative<std::monostate>(v)) {
          col.indicators[r] = backend::null_indicator;
        } else {
          col.i16_vals[r] = 0;
          col.indicators[r] = backend::null_indicator;
        }
      }
      break;
    case backend::buffer_type::int32:
      col.i32_vals.resize(n, 0);
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<std::int32_t>(&v)) {
          col.i32_vals[r] = *p;
          col.indicators[r] = sizeof(std::int32_t);
        } else if (std::holds_alternative<std::monostate>(v)) {
          col.indicators[r] = backend::null_indicator;
        } else {
          col.i32_vals[r] = 0;
          col.indicators[r] = backend::null_indicator;
        }
      }
      break;
    case backend::buffer_type::int64:
      col.i64_vals.resize(n, 0);
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<std::int64_t>(&v)) {
          col.i64_vals[r] = *p;
          col.indicators[r] = sizeof(std::int64_t);
        } else if (std::holds_alternative<std::monostate>(v)) {
          col.indicators[r] = backend::null_indicator;
        } else {
          col.i64_vals[r] = 0;
          col.indicators[r] = backend::null_indicator;
        }
      }
      break;
    case backend::buffer_type::float64:
      col.f64_vals.resize(n, 0.0);
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<double>(&v)) {
          col.f64_vals[r] = *p;
          col.indicators[r] = sizeof(double);
        } else if (std::holds_alternative<std::monostate>(v)) {
          col.indicators[r] = backend::null_indicator;
        } else {
          col.f64_vals[r] = 0.0;
          col.indicators[r] = backend::null_indicator;
        }
      }
      break;
    case backend::buffer_type::chars: {
      // Find max string length for stride
      std::size_t max_len = 1;
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<std::string>(&v)) {
          max_len = std::max(max_len, p->size());
        }
      }
      col.stride = max_len;
      col.var_buf.resize(n * max_len, '\0');
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<std::string>(&v)) {
          std::memcpy(col.var_buf.data() + r * max_len, p->data(), p->size());
          col.indicators[r] = static_cast<std::int64_t>(p->size());
        } else if (std::holds_alternative<std::monostate>(v)) {
          col.indicators[r] = backend::null_indicator;
        } else {
          col.indicators[r] = backend::null_indicator;
        }
      }
      break;
    }
    case backend::buffer_type::bytes: {
      std::size_t max_len = 1;
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<std::vector<std::byte>>(&v)) {
          max_len = std::max(max_len, p->size());
        }
      }
      col.stride = max_len;
      col.var_buf.resize(n * max_len, '\0');
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<std::vector<std::byte>>(&v)) {
          std::memcpy(
            col.var_buf.data() + r * max_len, p->data(), p->size());
          col.indicators[r] = static_cast<std::int64_t>(p->size());
        } else if (std::holds_alternative<std::monostate>(v)) {
          col.indicators[r] = backend::null_indicator;
        } else {
          col.indicators[r] = backend::null_indicator;
        }
      }
      break;
    }
    case backend::buffer_type::timestamp_parts:
      col.ts_vals.resize(n, {});
      for (std::size_t r = 0; r < n; ++r) {
        auto const& v = rows[start + r].at(c);
        if (auto* p = std::get_if<timestamp>(&v)) {
          auto parts = detail::break_timestamp(*p);
          col.ts_vals[r].year = static_cast<std::int16_t>(parts.year);
          col.ts_vals[r].month = static_cast<std::uint16_t>(parts.month);
          col.ts_vals[r].day = static_cast<std::uint16_t>(parts.day);
          col.ts_vals[r].hour = static_cast<std::uint16_t>(parts.hour);
          col.ts_vals[r].minute = static_cast<std::uint16_t>(parts.minute);
          col.ts_vals[r].second = static_cast<std::uint16_t>(parts.second);
          col.ts_vals[r].fraction_ns = parts.fraction_ns;
          col.indicators[r] = sizeof(backend::timestamp_parts);
        } else if (std::holds_alternative<std::monostate>(v)) {
          col.indicators[r] = backend::null_indicator;
        } else {
          col.ts_vals[r] = {};
          col.indicators[r] = backend::null_indicator;
        }
      }
      break;
    default:
      break;
    }
  }
}

void bind_batch_columns(backend::statement_iface& stmt,
  std::vector<batch_column>& cols) {
  for (std::size_t c = 0; c < cols.size(); ++c) {
    batch_column& col = cols[c];
    backend::param_array_buffer pab{};
    pab.type = col.type;
    pab.indicators = col.indicators.data();
    pab.count = col.count;

    switch (col.type) {
    case backend::buffer_type::bit:
      pab.data = col.bit_vals.data();
      pab.stride = sizeof(unsigned char);
      break;
    case backend::buffer_type::int16:
      pab.data = col.i16_vals.data();
      pab.stride = sizeof(std::int16_t);
      break;
    case backend::buffer_type::int32:
      pab.data = col.i32_vals.data();
      pab.stride = sizeof(std::int32_t);
      break;
    case backend::buffer_type::int64:
      pab.data = col.i64_vals.data();
      pab.stride = sizeof(std::int64_t);
      break;
    case backend::buffer_type::float64:
      pab.data = col.f64_vals.data();
      pab.stride = sizeof(double);
      break;
    case backend::buffer_type::chars:
    case backend::buffer_type::bytes:
      pab.data = col.var_buf.data();
      pab.stride = col.stride;
      break;
    case backend::buffer_type::timestamp_parts:
      pab.data = col.ts_vals.data();
      pab.stride = sizeof(backend::timestamp_parts);
      break;
    default:
      pab.data = col.var_buf.data();
      pab.stride = col.stride > 0 ? col.stride : 1;
      break;
    }

    stmt.bind_param_array(c + 1, pab);
  }
}

}  // namespace

std::size_t connection::insert_batch(std::string_view table,
  std::vector<std::string> const& columns, std::vector<params> const& rows) {
  if (rows.empty()) {
    return 0;
  }
  if (columns.empty()) {
    throw uniorm_error("insert_batch: no columns specified");
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].size() != columns.size()) {
      throw uniorm_error("insert_batch: row " + std::to_string(i) + " has " +
                         std::to_string(rows[i].size()) + " values, expected " +
                         std::to_string(columns.size()));
    }
  }

  dialect const d = dialect::detect(dbms_name());
  std::string sql = "INSERT INTO " + d.quote_identifier(table) + " (";
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i != 0) {
      sql += ", ";
    }
    sql += d.quote_identifier(columns[i]);
  }
  sql += ") VALUES (?";
  for (std::size_t i = 1; i < columns.size(); ++i) {
    sql += ", ?";
  }
  sql += ")";

  std::size_t batch_size = paramset_size_ > 0 ? paramset_size_
                                              : default_paramset_batch;
  std::size_t inserted = 0;
  transaction txn = begin();

  std::string key(sql);
  auto stmt = acquire_cached(key);

  // Pre-allocate column buffers outside the loop to avoid repeated allocations
  std::vector<batch_column> cols(columns.size());

  for (std::size_t start = 0; start < rows.size(); start += batch_size) {
    std::size_t end = std::min(start + batch_size, rows.size());
    std::size_t count = end - start;

    stmt->set_paramset_size(count);

    build_batch_columns(cols, rows, start, end, columns.size());
    bind_batch_columns(*stmt, cols);

    stmt->execute();
    inserted += stmt->affected_rows();
  }

  stmt_cache_->release(key, std::move(stmt));

  txn.commit();
  return inserted;
}

std::size_t connection::update_batch(std::string_view table,
  std::vector<std::string> const& set_columns,
  std::vector<std::string> const& where_columns,
  std::vector<params> const& rows) {
  if (rows.empty()) {
    return 0;
  }
  if (set_columns.empty()) {
    throw uniorm_error("update_batch: no SET columns specified");
  }
  if (where_columns.empty()) {
    throw uniorm_error("update_batch: no WHERE columns specified");
  }

  std::size_t total_columns = set_columns.size() + where_columns.size();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].size() != total_columns) {
      throw uniorm_error("update_batch: row " + std::to_string(i) + " has " +
                         std::to_string(rows[i].size()) + " values, expected " +
                         std::to_string(total_columns));
    }
  }

  dialect const d = dialect::detect(dbms_name());
  std::string sql = "UPDATE " + d.quote_identifier(table) + " SET ";
  for (std::size_t i = 0; i < set_columns.size(); ++i) {
    if (i != 0) {
      sql += ", ";
    }
    sql += d.quote_identifier(set_columns[i]) + " = ?";
  }
  sql += " WHERE ";
  for (std::size_t i = 0; i < where_columns.size(); ++i) {
    if (i != 0) {
      sql += " AND ";
    }
    sql += d.quote_identifier(where_columns[i]) + " = ?";
  }

  std::size_t batch_size = paramset_size_ > 0 ? paramset_size_
                                              : default_paramset_batch;
  std::size_t affected = 0;
  transaction txn = begin();

  std::string key(sql);
  auto stmt = acquire_cached(key);

  // Pre-allocate column buffers outside the loop
  std::vector<batch_column> cols(total_columns);

  for (std::size_t start = 0; start < rows.size(); start += batch_size) {
    std::size_t end = std::min(start + batch_size, rows.size());
    std::size_t count = end - start;

    stmt->set_paramset_size(count);

    build_batch_columns(cols, rows, start, end, total_columns);
    bind_batch_columns(*stmt, cols);

    stmt->execute();
    affected += stmt->affected_rows();
  }

  stmt_cache_->release(key, std::move(stmt));

  txn.commit();
  return affected;
}

std::size_t connection::remove_batch(std::string_view table,
  std::vector<std::string> const& where_columns,
  std::vector<params> const& keys) {
  if (keys.empty()) {
    return 0;
  }
  if (where_columns.empty()) {
    throw uniorm_error("remove_batch: no WHERE columns specified");
  }

  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (keys[i].size() != where_columns.size()) {
      throw uniorm_error("remove_batch: key " + std::to_string(i) + " has " +
                         std::to_string(keys[i].size()) + " values, expected " +
                         std::to_string(where_columns.size()));
    }
  }

  dialect const d = dialect::detect(dbms_name());
  std::string sql = "DELETE FROM " + d.quote_identifier(table) + " WHERE ";
  for (std::size_t i = 0; i < where_columns.size(); ++i) {
    if (i != 0) {
      sql += " AND ";
    }
    sql += d.quote_identifier(where_columns[i]) + " = ?";
  }

  std::size_t batch_size = paramset_size_ > 0 ? paramset_size_
                                              : default_paramset_batch;
  std::size_t affected = 0;
  transaction txn = begin();

  std::string key(sql);
  auto stmt = acquire_cached(key);

  // Pre-allocate column buffers outside the loop
  std::vector<batch_column> cols(where_columns.size());

  for (std::size_t start = 0; start < keys.size(); start += batch_size) {
    std::size_t end = std::min(start + batch_size, keys.size());
    std::size_t count = end - start;

    stmt->set_paramset_size(count);

    build_batch_columns(cols, keys, start, end, where_columns.size());
    bind_batch_columns(*stmt, cols);

    stmt->execute();
    affected += stmt->affected_rows();
  }

  stmt_cache_->release(key, std::move(stmt));

  txn.commit();
  return affected;
}

transaction connection::begin() {
  return transaction(*this);
}

std::string connection::dbms_name() const {
  return backend_->dbms_name();
}

void connection::set_autocommit(bool enabled) {
  backend_->set_autocommit(enabled);
}

void connection::commit() {
  backend_->commit();
}

void connection::rollback() {
  backend_->rollback();
}

}  // namespace uniorm
