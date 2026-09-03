#include "uniorm/result_set.hpp"

#include <cstring>
#include <utility>
#include <vector>

#include "uniorm/backend/backend.hpp"
#include "uniorm/detail/time.hpp"

namespace uniorm {

namespace {

enum class slot_kind { boolean, integer, floating, text, bytes, ts, dt, tm };

struct column_slot {
  column_info info;
  slot_kind kind = slot_kind::text;

  std::vector<unsigned char> bit_vals;
  std::vector<std::int64_t> int_vals;
  std::vector<double> dbl_vals;
  std::vector<char> text_buf;
  std::vector<std::byte> bin_buf;
  std::vector<backend::timestamp_parts> ts_vals;
  std::vector<backend::date_parts> date_vals;
  std::vector<backend::time_parts> time_vals;
  std::vector<std::int64_t> indicators;
};

slot_kind kind_for(sql_type type) {
  switch (type) {
  case sql_type::boolean:
    return slot_kind::boolean;
  case sql_type::smallint:
  case sql_type::integer:
  case sql_type::bigint:
    return slot_kind::integer;
  case sql_type::real:
  case sql_type::double_precision:
  case sql_type::decimal:
    return slot_kind::floating;
  case sql_type::binary:
  case sql_type::varbinary:
    return slot_kind::bytes;
  case sql_type::timestamp:
    return slot_kind::ts;
  case sql_type::date:
    return slot_kind::dt;
  case sql_type::time:
    return slot_kind::tm;
  default:
    return slot_kind::text;  // includes all char/wchar/guid variants
  }
}

}  // namespace

struct result_set::impl {
  std::unique_ptr<backend::statement_iface> stmt;
  std::vector<column_slot> slots;
  std::shared_ptr<column_names> names;
  std::function<void(std::unique_ptr<backend::statement_iface>)> release;
  std::size_t row_array_size_;
  std::size_t rows_fetched_ = 0;
  std::size_t current_row_ = 0;

  impl(std::unique_ptr<backend::statement_iface> s,
    std::function<void(std::unique_ptr<backend::statement_iface>)> r,
    std::size_t row_array_size)
    : stmt(std::move(s)), release(std::move(r)),
      // value_of() divides buffer sizes by this to locate a row's slot.
      row_array_size_(row_array_size > 0 ? row_array_size : 1) {
    describe_and_bind();
  }

  ~impl() {
    if (release) {
      release(std::move(stmt));
    }
  }

  void describe_and_bind() {
    stmt->set_row_array_size(row_array_size_);
    std::vector<column_info> meta = stmt->column_meta();
    slots.resize(meta.size());
    std::vector<std::string> collected;
    collected.reserve(meta.size());

    for (std::size_t i = 0; i < meta.size(); ++i) {
      column_slot& s = slots[i];
      s.info = meta[i];
      s.kind = kind_for(meta[i].type);

      backend::column_buffer buffer{};
      switch (s.kind) {
      case slot_kind::boolean:
        s.bit_vals.resize(row_array_size_);
        s.indicators.resize(row_array_size_);
        buffer = {backend::buffer_type::bit, s.bit_vals.data(),
          sizeof(unsigned char), s.indicators.data()};
        break;
      case slot_kind::integer:
        s.int_vals.resize(row_array_size_);
        s.indicators.resize(row_array_size_);
        buffer = {backend::buffer_type::int64, s.int_vals.data(),
          sizeof(std::int64_t), s.indicators.data()};
        break;
      case slot_kind::floating:
        s.dbl_vals.resize(row_array_size_);
        s.indicators.resize(row_array_size_);
        buffer = {backend::buffer_type::float64, s.dbl_vals.data(),
          sizeof(double), s.indicators.data()};
        break;
      case slot_kind::bytes: {
        std::size_t capacity =
          std::max<std::size_t>(meta[i].display_size, 32);
        s.bin_buf.resize(row_array_size_ * capacity);
        s.indicators.resize(row_array_size_);
        buffer = {backend::buffer_type::bytes, s.bin_buf.data(),
          capacity, s.indicators.data()};
        break;
      }
      case slot_kind::ts:
        s.ts_vals.resize(row_array_size_);
        s.indicators.resize(row_array_size_);
        buffer = {backend::buffer_type::timestamp_parts, s.ts_vals.data(),
          sizeof(backend::timestamp_parts), s.indicators.data()};
        break;
      case slot_kind::dt:
        s.date_vals.resize(row_array_size_);
        s.indicators.resize(row_array_size_);
        buffer = {backend::buffer_type::date_parts, s.date_vals.data(),
          sizeof(backend::date_parts), s.indicators.data()};
        break;
      case slot_kind::tm:
        s.time_vals.resize(row_array_size_);
        s.indicators.resize(row_array_size_);
        buffer = {backend::buffer_type::time_parts, s.time_vals.data(),
          sizeof(backend::time_parts), s.indicators.data()};
        break;
      case slot_kind::text:
      default: {
        std::size_t capacity =
          std::max<std::size_t>(meta[i].display_size, 31) + 1;
        s.text_buf.resize(row_array_size_ * capacity);
        s.indicators.resize(row_array_size_);
        buffer = {backend::buffer_type::chars, s.text_buf.data(),
          capacity, s.indicators.data()};
        break;
      }
      }

      stmt->bind_column(i + 1, buffer);
      collected.push_back(s.info.name);
    }

    names = std::make_shared<column_names>(std::move(collected));
  }

  sql_value value_of(std::size_t i, std::size_t row_index) {
    column_slot& s = slots[i];
    if (s.indicators[row_index] == backend::null_indicator) {
      return std::monostate{};
    }
    switch (s.kind) {
    case slot_kind::boolean:
      return s.bit_vals[row_index] != 0;
    case slot_kind::integer:
      return s.int_vals[row_index];
    case slot_kind::floating:
      return s.dbl_vals[row_index];
    case slot_kind::text: {
      std::size_t offset = row_index * (s.text_buf.size() / row_array_size_);
      std::int64_t ind = s.indicators[row_index];
      if (ind == backend::no_total ||
          ind > static_cast<std::int64_t>(s.text_buf.size() / row_array_size_) - 1) {
        return stmt->read_long_text(i + 1);
      }
      return std::string(
        s.text_buf.data() + offset, static_cast<std::size_t>(ind));
    }
    case slot_kind::bytes: {
      std::size_t capacity = s.bin_buf.size() / row_array_size_;
      std::size_t offset = row_index * capacity;
      std::int64_t ind = s.indicators[row_index];
      bool truncated = ind == backend::no_total ||
                       ind > static_cast<std::int64_t>(capacity);
      if (truncated) {
        return stmt->read_long_bytes(i + 1);
      }
      return std::vector<std::byte>(s.bin_buf.begin() + offset,
        s.bin_buf.begin() + offset + static_cast<std::ptrdiff_t>(ind));
    }
    case slot_kind::ts:
      return detail::make_timestamp(s.ts_vals[row_index].year,
        s.ts_vals[row_index].month, s.ts_vals[row_index].day,
        s.ts_vals[row_index].hour, s.ts_vals[row_index].minute,
        s.ts_vals[row_index].second, s.ts_vals[row_index].fraction_ns);
    case slot_kind::dt:
      return detail::make_timestamp(s.date_vals[row_index].year,
        s.date_vals[row_index].month, s.date_vals[row_index].day, 0, 0, 0, 0);
    case slot_kind::tm:
      return detail::make_timestamp(1970, 1, 1, s.time_vals[row_index].hour,
        s.time_vals[row_index].minute, s.time_vals[row_index].second, 0);
    }
    return std::monostate{};
  }
};

result_set::result_set(std::unique_ptr<impl> i) : impl_(std::move(i)) {}

result_set::~result_set() = default;

result_set::result_set(result_set&&) noexcept = default;

result_set& result_set::operator=(result_set&&) noexcept = default;

result_set result_set::from_statement(
  std::unique_ptr<backend::statement_iface> stmt,
  std::function<void(std::unique_ptr<backend::statement_iface>)> release,
  std::size_t row_array_size) {
  return result_set(
    std::make_unique<impl>(std::move(stmt), std::move(release), row_array_size));
}

bool result_set::next() {
  if (impl_->current_row_ < impl_->rows_fetched_) {
    ++impl_->current_row_;
    return true;
  }
  if (!impl_->stmt->fetch()) {
    return false;
  }
  impl_->rows_fetched_ = impl_->stmt->rows_fetched();
  impl_->current_row_ = 1;
  return impl_->rows_fetched_ > 0;
}

row result_set::current() {
  std::vector<sql_value> values;
  values.reserve(impl_->slots.size());
  for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
    values.push_back(impl_->value_of(i, impl_->current_row_ - 1));
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
