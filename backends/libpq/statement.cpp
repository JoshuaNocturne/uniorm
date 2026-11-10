#include "statement.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>

#include <uniorm/backend/error.hpp>
#include <uniorm/log.hpp>

#include "error.hpp"
#include "native_types.hpp"
#include "sql_text.hpp"

namespace uniorm::libpq {
namespace {

// One statement per object, under a name no other object holds: the server
// keeps the parse of a name it is given, which is what makes a cached
// statement cheaper than a fresh text.
std::string next_statement_name() {
  static std::atomic<std::size_t> counter{ 0 };
  return "uniorm_stmt_" + std::to_string(++counter);
}

// The server's word for a prepared statement it does not hold. A parse made
// inside a transaction that aborts goes away with it, and only this side's
// flag still says the name is live.
bool holds_no_parse(PGresult const* answer) {
  if (answer == nullptr) {
    return false;
  }
  char const* const state = PQresultErrorField(answer, PG_DIAG_SQLSTATE);
  return state != nullptr && std::strcmp(state, "26000") == 0;
}

char* byte_at(column_slot const& slot, std::size_t row) {
  return static_cast<char*>(slot.target.data) + row * slot.target.capacity;
}

// The most parameters one statement may declare: the extended protocol counts
// them in two bytes, and a row group of a folded insert carries one per
// column.
constexpr std::size_t k_parameter_ceiling = 65535;

// A value the server sent as text, read as the whole number it has to be: a
// run with anything left over after it is not a number at all.
std::int64_t whole_integer(char const* text) {
  if (text[0] != '\0' && text[1] == '\0') {
    // A bool, which the server spells one character long.
    if (text[0] == 't') {
      return 1;
    }
    if (text[0] == 'f') {
      return 0;
    }
  }
  errno = 0;
  char* stop = nullptr;
  long long const value = std::strtoll(text, &stop, 10);
  if (errno == ERANGE || stop == text || *stop != '\0') {
    throw pq_error("read '" + std::string(text) + "' as an integer", {});
  }
  return static_cast<std::int64_t>(value);
}

double whole_double(char const* text) {
  char* stop = nullptr;
  double const value = std::strtod(text, &stop);
  // A whole run of characters only: what the server left out of a value is a
  // width the caller did not bind for, not a rounding to be quietly made.
  if (stop == text || *stop != '\0') {
    throw pq_error("read '" + std::string(text) + "' as a floating value", {});
  }
  return value;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// A byte string as the server's hex form, which is what a bytea parameter is
// handed back as and what one is sent as.
std::string hex_text_of(std::vector<std::byte> const& bytes) {
  static char const digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2 + 2);
  out += "\\x";
  for (std::byte const one : bytes) {
    auto const byte = static_cast<unsigned char>(one);
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0x0f]);
  }
  return out;
}

// The bytes a value carries. A bytea arrives as the server's hex form, which
// leads with a backslash and carries two digits per byte; any other column's
// text is its own bytes.
std::vector<std::byte> bytes_of(
  char const* text, std::size_t length, bool hex_form) {
  if (!hex_form) {
    auto const* const first = reinterpret_cast<std::byte const*>(text);
    return std::vector<std::byte>(first, first + length);
  }
  if (length < 2 || text[0] != '\\' || text[1] != 'x' ||
      (length - 2) % 2 != 0) {
    throw pq_error("read a byte string the server did not send as hex", {});
  }
  std::vector<std::byte> out;
  out.reserve((length - 2) / 2);
  for (std::size_t index = 2; index + 1 < length; index += 2) {
    int const high = hex_value(text[index]);
    int const low = hex_value(text[index + 1]);
    if (high < 0 || low < 0) {
      throw pq_error("read a byte string that is not hex", {});
    }
    out.push_back(static_cast<std::byte>((high << 4) | low));
  }
  return out;
}

// The width a binding was made for, which is the range its value has to fit.
// A driver names an overflow rather than handing back the low bits it cut off,
// and two connectors over one server owe one answer for a mapping.
template <class Narrow>
Narrow as_width(std::int64_t value, std::size_t column, char const* text) {
  constexpr std::int64_t lowest = std::numeric_limits<Narrow>::min();
  constexpr std::int64_t highest = std::numeric_limits<Narrow>::max();
  if (value < lowest || value > highest) {
    throw pq_error("read result column " + std::to_string(column) + " as a " +
                     std::to_string(sizeof(Narrow) * 8) + "-bit integer: '" +
                     std::string(text) + "'",
      {});
  }
  return static_cast<Narrow>(value);
}

}  // namespace

pq_statement::pq_statement(connection& conn)
  : conn_(conn), statement_name_(next_statement_name()) {}

PGconn* pq_statement::session() const {
  PGconn* const made = conn_.native();
  if (made == nullptr) {
    throw pq_error("run a statement whose session has closed", {});
  }
  return made;
}

pq_statement::~pq_statement() {
  if (conn_.native() == nullptr) {
    return;
  }
  // This name is this statement's only hold on the server, and a cache that
  // drops an entry must not leave a parse behind in the session. A refusal
  // here says the session is gone, which is the same outcome.
  if (prepared_) {
    detail::result_holder dropped{ PQexec(
      conn_.native(), ("DEALLOCATE " + statement_name_).c_str()) };
    if (dropped.get() != nullptr &&
        PQresultStatus(dropped.get()) != PGRES_COMMAND_OK) {
      uniorm::detail::log(uniorm::log_level::warn, "libpq.statement", [&] {
        std::string text = "the session kept the parse " + statement_name_ +
                           " a statement let go of, and nobody was told";
        if (char const* const reason = PQresultErrorMessage(dropped.get());
          reason != nullptr) {
          text += ": " + std::string(reason);
        }
        return text;
      });
    }
  }
}

void pq_statement::prepare(std::string_view sql) {
  rewritten_ = rewrite_placeholders(sql, placeholder_count_);
  fold_ = find_values_fold(rewritten_, placeholder_count_);
  // The staged group was folded out of the text this statement held before.
  group_rows_ = 0;
  fold_text_.clear();
  prepare_under_name();
}

void pq_statement::prepare_under_name() {
  values_.assign(placeholder_count_, sql_value{});
  make_ready();
}

void pq_statement::make_ready() {
  if (prepared_) {
    // The server refuses to prepare over a name it already holds, and the
    // cache can hand this statement a second text.
    detail::result_holder dropped{ PQexec(
      session(), ("DEALLOCATE " + statement_name_).c_str()) };
    prepared_ = false;
  }
  param_types_.clear();
  detail::result_holder made{ PQprepare(session(), statement_name_.c_str(),
    rewritten_.c_str(), static_cast<int>(placeholder_count_), nullptr) };
  throw_if_failed(made.get(), "prepare a statement", { PGRES_COMMAND_OK });
  prepared_ = true;
  read_parameter_types();
}

void pq_statement::read_parameter_types() {
  // A describe of a prepared statement answers the types the server inferred
  // for its parameters, and those decide how each value is spelled: a date
  // takes different words than a timestamp, and a byte string takes hex.
  detail::result_holder shape{ PQdescribePrepared(
    session(), statement_name_.c_str()) };
  throw_if_failed(shape.get(), "read the parameters of a prepared statement",
    { PGRES_COMMAND_OK });
  param_types_.clear();
  int const declared = PQnparams(shape.get());
  for (int position = 0; position < declared; ++position) {
    param_types_.push_back(
      static_cast<unsigned>(PQparamtype(shape.get(), position)));
  }
}

void pq_statement::bind_parameter(std::size_t index, sql_value const& value) {
  if (index == 0 || index > placeholder_count_) {
    throw pq_error("bind parameter " + std::to_string(index) +
                     " of a statement that declares " +
                     std::to_string(placeholder_count_),
      {});
  }
  values_[index - 1] = value;
  batch_.clear();
}

void pq_statement::bind_column(
  std::size_t index, backend::column_buffer const& buffer) {
  // With no result the select list has no length, so every column the caller
  // names is past it.
  if (index == 0 || index > slots_.size()) {
    throw pq_error(
      "result column " + std::to_string(index) + " is past the select list",
      {});
  }
  // The core binds its fields in whatever order it mapped them. A column
  // handed over twice would be written twice into buffers of different widths.
  column_slot& slot = slots_[index - 1];
  if (slot.bound) {
    throw pq_error(
      "bind result column " + std::to_string(index) + " a second time", {});
  }
  slot.target = buffer;
  slot.position = index;
  slot.bound = true;
}

void pq_statement::bind_batch_params(std::vector<params> const& rows) {
  // Staged here and sent by execute(): this backend offers no columnar
  // channel. Row by row rather than batch_ = rows: params' variadic
  // constructor outranks its implicit copy for a non-const lvalue, and a
  // vector assignment reaches for it.
  batch_.clear();
  batch_.reserve(rows.size());
  for (params const& row : rows) {
    batch_.push_back(row);
  }
  values_.assign(placeholder_count_, sql_value{});
}

backend::batch_writer_iface& pq_statement::prepare_batch() {
  throw backend::capability_not_supported(
    "direct-write batch binding is not supported");
}

void pq_statement::render(std::vector<sql_value> const& values) {
  param_text_.clear();
  param_pointers_.clear();
  sent_types_.clear();
  // The pointers libpq reads go into the strings staged here, so neither
  // vector may move after this reserve: a short value living inside its own
  // string would otherwise change address with the next parameter.
  param_text_.reserve(values.size());
  param_pointers_.reserve(values.size());
  sent_types_.reserve(values.size());
  // A folded batch carries the one row's parameters several times over, and
  // the describe of the one-row text is the only reading of their types this
  // statement has: each group repeats what that answer said.
  for (std::size_t index = 0; index < values.size(); ++index) {
    sql_value const& value = values[index];
    sent_types_.push_back(index < param_types_.size()
                            ? param_types_[index % param_types_.size()]
                            : 0);
    if (is_null(value)) {
      param_pointers_.push_back(nullptr);
      continue;
    }
    param_text_.push_back(render_one(value, sent_types_.back()));
    param_pointers_.push_back(param_text_.back().c_str());
  }
}

std::string pq_statement::render_one(
  sql_value const& value, unsigned type) const {
  return std::visit(
    [&](auto const& held) -> std::string {
      using kind = std::decay_t<decltype(held)>;
      if constexpr (std::is_same_v<kind, std::monostate>) {
        return std::string();
      } else if constexpr (std::is_same_v<kind, bool>) {
        // The server reads both spellings of a bool, and these two of every
        // integer column a bool may be mapped onto.
        return held ? "1" : "0";
      } else if constexpr (std::is_same_v<kind, std::string>) {
        return held;
      } else if constexpr (std::is_same_v<kind, std::vector<std::byte>>) {
        return hex_text_of(held);
      } else if constexpr (std::is_same_v<kind, timestamp>) {
        return text_of_timestamp(held, type);
      } else if constexpr (std::is_same_v<kind, double>) {
        char buffer[64] = {};
        // Enough digits to land back on the same value, and no more: %g drops
        // the zeros a whole number does not need.
        std::snprintf(buffer, sizeof buffer, "%.17g", held);
        return buffer;
      } else {
        return std::to_string(held);
      }
    },
    value);
}

PGresult* pq_statement::run() {
  // Text in both directions: the parameters are the words the server's own
  // input functions take, and a result read back as text is the same words,
  // which keeps an exact number exact.
  if (folding_) {
    return PQexecParams(session(), fold_text_.c_str(),
      static_cast<int>(param_pointers_.size()), sent_types_.data(),
      param_pointers_.empty() ? nullptr : param_pointers_.data(), nullptr,
      nullptr, 0);
  }
  return PQexecPrepared(session(), statement_name_.c_str(),
    static_cast<int>(param_pointers_.size()),
    param_pointers_.empty() ? nullptr : param_pointers_.data(), nullptr,
    nullptr, 0);
}

detail::result_holder pq_statement::run_ready() {
  detail::result_holder next{ run() };
  if (prepared_ && holds_no_parse(next.get())) {
    // The name is this statement's own, so an answer saying the server holds
    // no such parse can only mean the session lost it -- a session that did not
    // keep the server connection it was made on, or a DEALLOCATE from outside.
    // Making it again is the only reading of that answer. A folded group takes
    // no name, so it never hears this.
    uniorm::detail::log(uniorm::log_level::warn, "libpq.statement", [&] {
      return "the session no longer held the parse " + statement_name_ +
             ", so it was made again";
    });
    make_ready();
    render(values_);
    next = detail::result_holder{ run() };
  }
  return next;
}

void pq_statement::execute() {
  conn_.open_transaction();
  folding_ = false;
  if (!batch_.empty()) {
    run_batch();
    return;
  }
  render(values_);
  apply(run_ready());
  values_.assign(placeholder_count_, sql_value{});
}

void pq_statement::run_batch() {
  // Taken out of the member before the walk: a row that fails would otherwise
  // leave the whole batch here for a replay that counts every row twice.
  std::vector<params> rows;
  rows.swap(batch_);
  if (rows.size() > 1 && fold_.columns > 0 &&
      fold_.columns <= k_parameter_ceiling) {
    run_folded(rows);
    return;
  }
  folding_ = false;
  std::size_t touched = 0;
  for (params const& row : rows) {
    values_.assign(row.values().begin(), row.values().end());
    render(values_);
    detail::result_holder next{ run_ready() };
    throw_if_failed(next.get(), "execute a prepared statement",
      { PGRES_COMMAND_OK, PGRES_TUPLES_OK });
    touched += affected_of(next.get());
    // The last answer is the one the metadata reads are made against.
    result_ = std::move(next);
  }
  settle(touched);
}

void pq_statement::run_folded(std::vector<params> const& rows) {
  // The most rows one execute can carry: the protocol counts a statement's
  // parameters in two bytes, and a group of them has one per column.
  std::size_t const columns = fold_.columns;
  std::size_t const ceiling = k_parameter_ceiling / columns;
  // Groups of one size rather than full ones and a short tail: the text is
  // written out for the count it carries, so a tail of its own length would
  // make a second text and leave a repeating sweep alternating between them.
  std::size_t const groups = (rows.size() + ceiling - 1) / ceiling;
  std::size_t const per_group = (rows.size() + groups - 1) / groups;
  folding_ = true;
  std::size_t touched = 0;
  for (std::size_t start = 0; start < rows.size(); start += per_group) {
    std::size_t const count = (std::min)(per_group, rows.size() - start);
    if (group_rows_ != count) {
      fold_text_ = expand_values_fold(fold_, count);
      group_rows_ = count;
    }
    values_.clear();
    for (std::size_t row = start; row < start + count; ++row) {
      std::vector<sql_value> const& values = rows[row].values();
      values_.insert(values_.end(), values.begin(), values.end());
    }
    render(values_);
    detail::result_holder next{ run() };
    throw_if_failed(next.get(), "execute a folded statement",
      { PGRES_COMMAND_OK, PGRES_TUPLES_OK });
    touched += affected_of(next.get());
    result_ = std::move(next);
  }
  folding_ = false;
  // The group's values are staged in the member the one-row path keeps its
  // own bind in, which is more values than that text declares.
  values_.assign(placeholder_count_, sql_value{});
  settle(touched);
}

void pq_statement::settle(std::size_t touched) {
  query_ = PQresultStatus(result_.get()) == PGRES_TUPLES_OK;
  affected_ = touched;
  cursor_ = 0;
  block_start_ = 0;
  rows_fetched_ = 0;
  declare_shape();
}

void pq_statement::apply(detail::result_holder&& next) {
  throw_if_failed(next.get(), "execute a prepared statement",
    { PGRES_COMMAND_OK, PGRES_TUPLES_OK });
  result_ = std::move(next);
  settle(affected_of(result_.get()));
}

void pq_statement::declare_shape() {
  int const fields = result_.get() == nullptr ? 0 : PQnfields(result_.get());
  slots_.assign(
    static_cast<std::size_t>(fields < 0 ? 0 : fields), column_slot{});
}

std::size_t pq_statement::affected_of(PGresult* answer) const {
  // The server counts the rows a write touched under its own command tag, and
  // leaves the word empty for everything else, a query included.
  char const* const counted = PQcmdTuples(answer);
  if (counted == nullptr || counted[0] == '\0') {
    return 0;
  }
  return static_cast<std::size_t>(std::strtoull(counted, nullptr, 10));
}

bool pq_statement::fetch() {
  if (!query_ || result_.get() == nullptr) {
    rows_fetched_ = 0;
    return false;
  }
  std::size_t const total = static_cast<std::size_t>(PQntuples(result_.get()));
  if (cursor_ >= total) {
    rows_fetched_ = 0;
    return false;
  }
  std::size_t const block = (std::min)(row_array_size_, total - cursor_);
  block_start_ = cursor_;
  cursor_ += block;
  rows_fetched_ = block;
  materialize(block);
  return true;
}

void pq_statement::materialize(std::size_t rows) {
  for (column_slot& slot : slots_) {
    if (!slot.bound) {
      continue;
    }
    for (std::size_t row = 0; row < rows; ++row) {
      fill_cell(slot, row);
    }
  }
}

void pq_statement::fill_cell(column_slot& slot, std::size_t row) {
  PGresult const* const answer = result_.get();
  int const field = static_cast<int>(slot.position) - 1;
  int const tuple = static_cast<int>(block_start_ + row);
  if (PQgetisnull(answer, tuple, field) == 1) {
    slot.target.indicator[row] = backend::null_indicator;
    return;
  }
  slot.target.indicator[row] = 0;
  char const* const text = PQgetvalue(answer, tuple, field);
  std::size_t const length =
    static_cast<std::size_t>(PQgetlength(answer, tuple, field));
  switch (slot.target.type) {
  case backend::buffer_type::chars:
    put_text(slot, row, text, length);
    return;
  case backend::buffer_type::bytes: {
    bool const hex_form = PQftype(answer, field) == pg_type::bytea;
    std::vector<std::byte> const held = bytes_of(text, length, hex_form);
    auto const* const bytes = reinterpret_cast<char const*>(held.data());
    put_bytes(slot, row, bytes, held.size());
    return;
  }
  case backend::buffer_type::bit:
  case backend::buffer_type::int8:
  case backend::buffer_type::int16:
  case backend::buffer_type::int32:
  case backend::buffer_type::int64:
    put_integer(slot, row, text);
    return;
  case backend::buffer_type::float32:
  case backend::buffer_type::float64:
    put_double(slot, row, text);
    return;
  case backend::buffer_type::timestamp_parts:
  case backend::buffer_type::date_parts:
  case backend::buffer_type::time_parts:
    put_timestamp(slot, row, text, PQftype(answer, field));
    return;
  }
}

// Copies one text value into the caller's row slot and records the length the
// caller's convention reads back. A character binding reserves its last byte
// for a terminator, so it reads as truncated one byte before a binary one
// does.
void pq_statement::put_text(
  column_slot& slot, std::size_t row, char const* data, std::size_t length) {
  std::size_t const capacity = slot.target.capacity;
  std::size_t const limit = capacity > 0 ? capacity - 1 : 0;
  std::size_t const taken = (std::min)(length, limit);
  std::memcpy(byte_at(slot, row), data, taken);
  slot.target.indicator[row] = taken < length
                                 ? static_cast<std::int64_t>(limit) + 1
                                 : static_cast<std::int64_t>(length);
}

void pq_statement::put_bytes(
  column_slot& slot, std::size_t row, char const* data, std::size_t length) {
  std::size_t const capacity = slot.target.capacity;
  std::size_t const taken = (std::min)(length, capacity);
  std::memcpy(byte_at(slot, row), data, taken);
  slot.target.indicator[row] = taken < length
                                 ? static_cast<std::int64_t>(capacity) + 1
                                 : static_cast<std::int64_t>(length);
}

void pq_statement::put_integer(
  column_slot& slot, std::size_t row, char const* text) const {
  std::int64_t const value = whole_integer(text);
  char* const destination = byte_at(slot, row);
  switch (slot.target.type) {
  case backend::buffer_type::bit:
  case backend::buffer_type::int8:
    *reinterpret_cast<std::int8_t*>(destination) =
      as_width<std::int8_t>(value, slot.position, text);
    return;
  case backend::buffer_type::int16:
    *reinterpret_cast<std::int16_t*>(destination) =
      as_width<std::int16_t>(value, slot.position, text);
    return;
  case backend::buffer_type::int32:
    *reinterpret_cast<std::int32_t*>(destination) =
      as_width<std::int32_t>(value, slot.position, text);
    return;
  default:
    *reinterpret_cast<std::int64_t*>(destination) = value;
    return;
  }
}

void pq_statement::put_double(
  column_slot& slot, std::size_t row, char const* text) const {
  double const value = whole_double(text);
  char* const destination = byte_at(slot, row);
  if (slot.target.type == backend::buffer_type::float32) {
    *reinterpret_cast<float*>(destination) = static_cast<float>(value);
    return;
  }
  *reinterpret_cast<double*>(destination) = value;
}

void pq_statement::put_timestamp(
  column_slot& slot, std::size_t row, char const* text, unsigned native) const {
  auto const parts = parts_of(text, native);
  char* const destination = byte_at(slot, row);
  if (slot.target.type == backend::buffer_type::date_parts) {
    auto* const out = reinterpret_cast<backend::date_parts*>(destination);
    out->year = parts.year;
    out->month = parts.month;
    out->day = parts.day;
    return;
  }
  if (slot.target.type == backend::buffer_type::time_parts) {
    auto* const out = reinterpret_cast<backend::time_parts*>(destination);
    out->hour = parts.hour;
    out->minute = parts.minute;
    out->second = parts.second;
    return;
  }
  *reinterpret_cast<backend::timestamp_parts*>(destination) = parts;
}

char const* pq_statement::cell(
  std::size_t column, std::size_t row, std::size_t& length) const {
  PGresult const* const answer = result_.get();
  // The read names a row of the block just fetched, and the result holds the
  // whole set, so the block's start is what turns one into the other.
  if (answer == nullptr || column == 0 ||
      static_cast<int>(column) > PQnfields(answer)) {
    throw pq_error(
      "read a value of result column " + std::to_string(column), {});
  }
  int const tuple = static_cast<int>(block_start_ + row);
  int const field = static_cast<int>(column) - 1;
  if (tuple >= PQntuples(answer) || PQgetisnull(answer, tuple, field) == 1) {
    length = 0;
    return "";
  }
  length = static_cast<std::size_t>(PQgetlength(answer, tuple, field));
  return PQgetvalue(answer, tuple, field);
}

column_slot& pq_statement::slot_for(std::size_t column) {
  for (column_slot& slot : slots_) {
    if (slot.bound && slot.position == column) {
      return slot;
    }
  }
  throw pq_error("read a value of result column " + std::to_string(column), {});
}

std::string pq_statement::read_long_text(std::size_t column, std::size_t row) {
  slot_for(column);
  std::size_t length = 0;
  char const* const text = cell(column, row, length);
  return std::string(text, length);
}

std::vector<std::byte> pq_statement::read_long_bytes(
  std::size_t column, std::size_t row) {
  slot_for(column);
  std::size_t length = 0;
  char const* const text = cell(column, row, length);
  bool const hex_form =
    PQftype(result_.get(), static_cast<int>(column) - 1) == pg_type::bytea;
  return bytes_of(text, length, hex_form);
}

std::size_t pq_statement::affected_rows() const {
  return affected_;
}

std::vector<column_info> pq_statement::column_meta() const {
  std::vector<column_info> out;
  PGresult const* const answer = result_.get();
  if (answer == nullptr) {
    return out;
  }
  int const fields = PQnfields(answer);
  out.reserve(static_cast<std::size_t>(fields));
  for (int field = 0; field < fields; ++field) {
    column_info info;
    char const* const name = PQfname(answer, field);
    info.name = name == nullptr ? std::string() : name;
    unsigned const native = static_cast<unsigned>(PQftype(answer, field));
    int const modifier = PQfmod(answer, field);
    info.type = sql_type_from_native(native);
    info.display_size = display_width(info.type, native, modifier);
    info.scale = native == pg_type::numeric
                   ? static_cast<std::size_t>(declared_scale(modifier))
                   : 0;
    // A result says nothing about the column behind it, and a value the server
    // never sent is the only case a NULL test would have caught: the read of
    // each row carries its own.
    info.nullable = true;
    out.push_back(std::move(info));
  }
  return out;
}

void pq_statement::set_row_array_size(std::size_t size) {
  row_array_size_ = size == 0 ? 1 : size;
}

std::size_t pq_statement::rows_fetched() const {
  return rows_fetched_;
}

std::size_t pq_statement::result_row_estimate() const {
  if (!query_ || result_.get() == nullptr) {
    return 0;
  }
  return static_cast<std::size_t>(PQntuples(result_.get()));
}

void pq_statement::set_paramset_size(std::size_t) {
  // Ignored: bind_batch_params() hands over the whole batch and run_batch()
  // walks it, so the count needs no second telling.
}

void pq_statement::reset() {
  // A cached statement is about to be rebound for the same text: the rows it
  // still holds are not the ones the next read wants, and a result keeps its
  // memory until the server's words are given back.
  result_.clear();
  slots_.clear();
  batch_.clear();
  values_.assign(placeholder_count_, sql_value{});
  cursor_ = 0;
  block_start_ = 0;
  rows_fetched_ = 0;
  affected_ = 0;
  query_ = false;
}

}  // namespace uniorm::libpq
