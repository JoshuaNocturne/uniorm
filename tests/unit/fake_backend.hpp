#pragma once

// In-memory backend implementing uniorm::backend. Used by unit tests to
// prove the core API is backend-neutral: the test target compiles and
// links without any ODBC headers or libraries.
//
// Usage from tests:
//   uniorm::connection conn("fake://ignored");
//   auto* fake = conn.native_handle<uniorm::test::fake_connection>();
//   fake->script_next(...);           // result for the next execute()
//   auto rs = conn.execute(sql);

#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/registry.hpp>

namespace uniorm::test {

struct fake_counters {
  int statements_created = 0;
  int fetch_calls = 0;
  int reset_calls = 0;
};

class fake_statement : public backend::statement_iface {
public:
  // One scripted result column; values are copied into bound buffers on
  // fetch (std::monostate -> NULL indicator).
  struct scripted_column {
    column_info info;
    std::vector<sql_value> values;  // one entry per scripted row
  };

  struct scripted_result {
    std::vector<scripted_column> columns;
    std::size_t affected = 0;
  };

  fake_statement(fake_counters& counters, std::deque<scripted_result>& pending)
    : counters_(counters), pending_(pending) {}

  void prepare(std::string_view sql) override {
    sql_ = std::string(sql);
  }

  void bind_parameter(std::size_t index, sql_value const& value) override {
    if (bound_params_.size() < index) {
      bound_params_.resize(index);
    }
    bound_params_[index - 1] = value;
  }

  void bind_column(
    std::size_t index, backend::column_buffer const& buffer) override {
    if (bound_columns_.size() < index) {
      bound_columns_.resize(index);
    }
    bound_columns_[index - 1] = buffer;
  }

  void execute() override {
    executed_ = true;
    cursor_ = 0;
    columns_.clear();
    result_rows_.clear();
    affected_ = 0;
    if (!pending_.empty()) {
      scripted_result r = std::move(pending_.front());
      pending_.pop_front();
      affected_ = r.affected;
      std::size_t rows =
        r.columns.empty() ? 0 : r.columns.front().values.size();
      for (auto const& c : r.columns) {
        columns_.push_back(c.info);
      }
      for (std::size_t i = 0; i < rows; ++i) {
        std::vector<sql_value> row;
        row.reserve(r.columns.size());
        for (auto const& c : r.columns) {
          row.push_back(c.values.at(i));
        }
        result_rows_.push_back(std::move(row));
      }
    }
  }

  bool fetch() override {
    ++counters_.fetch_calls;
    if (cursor_ >= result_rows_.size()) {
      return false;
    }
    for (std::size_t i = 0;
      i < bound_columns_.size() && i < result_rows_[cursor_].size(); ++i) {
      write_buffer(bound_columns_[i], result_rows_[cursor_][i]);
    }
    ++cursor_;
    return true;
  }

  std::size_t affected_rows() const override {
    return affected_;
  }

  std::vector<column_info> column_meta() const override {
    return columns_;
  }

  std::string read_long_text(std::size_t) override {
    return long_text_;
  }
  std::vector<std::byte> read_long_bytes(std::size_t) override {
    return long_bytes_;
  }

  void reset() override {
    ++counters_.reset_calls;
    // Statements are reset when checked back into the cache; keep a copy
    // of the last bound parameters so tests can assert on them afterwards.
    last_params_ = bound_params_;
    bound_params_.clear();
    bound_columns_.clear();
    executed_ = false;
    cursor_ = 0;
  }

  std::string const& prepared_sql() const {
    return sql_;
  }
  std::vector<sql_value> const& bound_params() const {
    return bound_params_;
  }
  std::vector<sql_value> const& last_params() const {
    return last_params_;
  }

private:
  static void write_buffer(
    backend::column_buffer const& b, sql_value const& v) {
    if (std::holds_alternative<std::monostate>(v)) {
      *b.indicator = backend::null_indicator;
      return;
    }
    switch (b.type) {
    case backend::buffer_type::bit: {
      auto* p = static_cast<unsigned char*>(b.data);
      *p = std::get<bool>(v) ? 1 : 0;
      *b.indicator = 1;
      break;
    }
    case backend::buffer_type::int8: {
      auto* p = static_cast<std::int8_t*>(b.data);
      *p = static_cast<std::int8_t>(std::get<std::int64_t>(v));
      *b.indicator = static_cast<std::int64_t>(sizeof(*p));
      break;
    }
    case backend::buffer_type::int16: {
      auto* p = static_cast<std::int16_t*>(b.data);
      *p = static_cast<std::int16_t>(std::get<std::int64_t>(v));
      *b.indicator = static_cast<std::int64_t>(sizeof(*p));
      break;
    }
    case backend::buffer_type::int32: {
      auto* p = static_cast<std::int32_t*>(b.data);
      *p = static_cast<std::int32_t>(std::get<std::int64_t>(v));
      *b.indicator = static_cast<std::int64_t>(sizeof(*p));
      break;
    }
    case backend::buffer_type::int64: {
      auto* p = static_cast<std::int64_t*>(b.data);
      *p = std::get<std::int64_t>(v);
      *b.indicator = static_cast<std::int64_t>(sizeof(*p));
      break;
    }
    case backend::buffer_type::float32: {
      auto* p = static_cast<float*>(b.data);
      *p = static_cast<float>(std::get<double>(v));
      *b.indicator = static_cast<std::int64_t>(sizeof(*p));
      break;
    }
    case backend::buffer_type::float64: {
      auto* p = static_cast<double*>(b.data);
      *p = std::get<double>(v);
      *b.indicator = static_cast<std::int64_t>(sizeof(*p));
      break;
    }
    case backend::buffer_type::chars: {
      auto const& s = std::get<std::string>(v);
      std::size_t take = std::min(s.size(), b.capacity - 1);
      std::memcpy(b.data, s.data(), take);
      static_cast<char*>(b.data)[take] = '\0';
      // Indicator reports the full value length; the core treats values
      // longer than capacity-1 as truncated and calls read_long_text.
      *b.indicator = static_cast<std::int64_t>(s.size());
      break;
    }
    case backend::buffer_type::bytes: {
      auto const& bytes = std::get<std::vector<std::byte>>(v);
      std::size_t take = std::min(bytes.size(), b.capacity);
      std::memcpy(b.data, bytes.data(), take);
      *b.indicator = static_cast<std::int64_t>(bytes.size());
      break;
    }
    case backend::buffer_type::timestamp_parts:
    case backend::buffer_type::date_parts:
    case backend::buffer_type::time_parts:
      // Not exercised by the fake-backend tests.
      *b.indicator = backend::null_indicator;
      break;
    }
  }

  fake_counters& counters_;
  std::deque<scripted_result>& pending_;
  std::string sql_;
  bool executed_ = false;
  std::size_t affected_ = 0;
  std::size_t cursor_ = 0;
  std::vector<sql_value> bound_params_;
  std::vector<sql_value> last_params_;
  std::vector<backend::column_buffer> bound_columns_;
  std::vector<column_info> columns_;
  std::vector<std::vector<sql_value>> result_rows_;
  std::string long_text_;
  std::vector<std::byte> long_bytes_;
};

class fake_connection : public backend::connection_iface {
public:
  using scripted_result = fake_statement::scripted_result;
  using scripted_column = fake_statement::scripted_column;

  void open(std::string_view connection_string) override {
    opened_with_ = std::string(connection_string);
    open_ = true;
  }
  void close() override {
    open_ = false;
  }
  bool is_open() const noexcept override {
    return open_;
  }
  void set_autocommit(bool enabled) override {
    autocommit_log_.push_back(enabled);
  }
  void commit() override {
    ++commits_;
  }
  void rollback() override {
    ++rollbacks_;
  }
  backend::capabilities caps() const noexcept override {
    return {};
  }
  std::string dbms_name() const override {
    return "FakeDB";
  }
  std::unique_ptr<backend::statement_iface> create_statement() override {
    ++counters_.statements_created;
    auto stmt = std::make_unique<fake_statement>(counters_, pending_);
    last_statement_ = stmt.get();
    return stmt;
  }
  void* native_handle() noexcept override {
    return this;
  }

  // Queue the result/affected count for the next executed statement.
  void script_next(scripted_result r) {
    pending_.push_back(std::move(r));
  }

  fake_counters counters_;
  std::string opened_with_;
  std::vector<bool> autocommit_log_;
  int commits_ = 0;
  int rollbacks_ = 0;
  fake_statement* last_statement_ = nullptr;

private:
  bool open_ = false;
  std::deque<scripted_result> pending_;
};

// Registers the "fake" scheme once per process; including this header is
// enough to make "fake://" connection strings work.
inline backend::registrar const& fake_registrar() {
  static backend::registrar const reg(
    "fake", [] { return std::make_unique<fake_connection>(); });
  return reg;
}

struct fake_backend_init {
  fake_backend_init() {
    fake_registrar();
  }
};
inline fake_backend_init const fake_backend_init_instance{};

}  // namespace uniorm::test
