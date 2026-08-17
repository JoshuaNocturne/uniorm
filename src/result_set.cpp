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

  unsigned char bit_val = 0;
  std::int64_t int_val = 0;
  double dbl_val = 0.0;
  std::vector<char> text_buf;
  std::vector<std::byte> bin_buf;
  backend::timestamp_parts ts_val{};
  backend::date_parts date_val{};
  backend::time_parts time_val{};
  std::int64_t indicator = 0;
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

  impl(std::unique_ptr<backend::statement_iface> s,
    std::function<void(std::unique_ptr<backend::statement_iface>)> r)
    : stmt(std::move(s)), release(std::move(r)) {
    describe_and_bind();
  }

  ~impl() {
    if (release) {
      release(std::move(stmt));
    }
  }

  void describe_and_bind() {
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
        buffer = {backend::buffer_type::bit, &s.bit_val, sizeof(s.bit_val),
          &s.indicator};
        break;
      case slot_kind::integer:
        buffer = {backend::buffer_type::int64, &s.int_val, sizeof(s.int_val),
          &s.indicator};
        break;
      case slot_kind::floating:
        buffer = {backend::buffer_type::float64, &s.dbl_val,
          sizeof(s.dbl_val), &s.indicator};
        break;
      case slot_kind::bytes: {
        std::size_t capacity =
          std::max<std::size_t>(meta[i].display_size, 32);
        s.bin_buf.resize(capacity);
        buffer = {backend::buffer_type::bytes, s.bin_buf.data(),
          s.bin_buf.size(), &s.indicator};
        break;
      }
      case slot_kind::ts:
        buffer = {backend::buffer_type::timestamp_parts, &s.ts_val,
          sizeof(s.ts_val), &s.indicator};
        break;
      case slot_kind::dt:
        buffer = {backend::buffer_type::date_parts, &s.date_val,
          sizeof(s.date_val), &s.indicator};
        break;
      case slot_kind::tm:
        buffer = {backend::buffer_type::time_parts, &s.time_val,
          sizeof(s.time_val), &s.indicator};
        break;
      case slot_kind::text:
      default: {
        std::size_t capacity =
          std::max<std::size_t>(meta[i].display_size, 31) + 1;
        s.text_buf.resize(capacity);
        buffer = {backend::buffer_type::chars, s.text_buf.data(),
          s.text_buf.size(), &s.indicator};
        break;
      }
      }

      stmt->bind_column(i + 1, buffer);
      collected.push_back(s.info.name);
    }

    names = std::make_shared<column_names>(std::move(collected));
  }

  sql_value value_of(std::size_t i) {
    column_slot& s = slots[i];
    if (s.indicator == backend::null_indicator) {
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
      if (s.indicator == backend::no_total ||
          s.indicator > static_cast<std::int64_t>(s.text_buf.size()) - 1) {
        return stmt->read_long_text(i + 1);
      }
      return std::string(
        s.text_buf.data(), static_cast<std::size_t>(s.indicator));
    }
    case slot_kind::bytes: {
      bool truncated = s.indicator == backend::no_total ||
                       s.indicator >
                         static_cast<std::int64_t>(s.bin_buf.size());
      if (truncated) {
        return stmt->read_long_bytes(i + 1);
      }
      return std::vector<std::byte>(s.bin_buf.begin(),
        s.bin_buf.begin() + static_cast<std::ptrdiff_t>(s.indicator));
    }
    case slot_kind::ts:
      return detail::make_timestamp(s.ts_val.year, s.ts_val.month,
        s.ts_val.day, s.ts_val.hour, s.ts_val.minute, s.ts_val.second,
        s.ts_val.fraction_ns);
    case slot_kind::dt:
      return detail::make_timestamp(
        s.date_val.year, s.date_val.month, s.date_val.day, 0, 0, 0, 0);
    case slot_kind::tm:
      return detail::make_timestamp(1970, 1, 1, s.time_val.hour,
        s.time_val.minute, s.time_val.second, 0);
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
  std::function<void(std::unique_ptr<backend::statement_iface>)> release) {
  return result_set(
    std::make_unique<impl>(std::move(stmt), std::move(release)));
}

bool result_set::next() {
  return impl_->stmt->fetch();
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
