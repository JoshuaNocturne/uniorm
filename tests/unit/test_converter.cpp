// Converter extension point: the value layer encodes a domain type through
// to_db and decodes it through from_db. These cases pin the ordering that
// makes the feature usable at all -- an adapter must win over the implicit
// enum-to-integer and implicit-string-conversion arms, which bind a domain
// type to something other than what its converter says -- and the read path
// that stages a column as that representation before decoding it.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/builder/expression.hpp>
#include <uniorm/converter.hpp>
#include <uniorm/detail/projection.hpp>
#include <uniorm/params.hpp>

#include "check.hpp"

using namespace uniorm;

namespace {

enum class status { fresh, paid, shipped };

// No converter: kept to show that the pre-existing enum arm is untouched.
enum class plain_enum { three = 3 };

struct money {
  std::int64_t units;
  std::int64_t micros;
};

// Has an implicit conversion to std::string, which is exactly the arm that
// would otherwise claim it.
struct tagged {
  int code;

  operator std::string() const {
    return "via-implicit";
  }
};

struct record {
  std::int64_t id;
  status state;
};

// Declared with only sql and to_db: detection must reject it rather than fail
// hard, so a forgotten from_db surfaces as "unsupported type" downstream.
struct half_wired {
  int value;
};

// Read-path layouts: the projection layer binds one column per field.
struct read_row {
  std::int64_t id;
  status state;
  std::optional<status> note;
};

struct code_row {
  tagged tag;
};

// Records the buffers the projection layer binds so a test can write the
// values a fetch would have staged, with no driver in the picture.
class fake_statement : public backend::statement_iface {
public:
  using buffer = backend::column_buffer;

  explicit fake_statement(std::vector<column_info> meta)
    : meta_(std::move(meta)),
      long_text_(meta_.size()) {}

  void bind_column(std::size_t column, buffer const& bound) override {
    if (column == 0 || bound_.size() < column) bound_.resize(column);
    bound_[column - 1] = bound;
  }

  backend::buffer_type kind(std::size_t column) const {
    return bound_[column - 1].type;
  }

  std::size_t stride(std::size_t column) const {
    return bound_[column - 1].capacity;
  }

  template <class T>
  void put(std::size_t column, std::size_t row, T value) {
    buffer& bound = bound_[column - 1];
    *reinterpret_cast<T*>(
      static_cast<char*>(bound.data) + row * bound.capacity) = value;
    bound.indicator[row] = static_cast<std::int64_t>(sizeof(T));
  }

  void put_text(std::size_t column, std::size_t row, std::string const& text) {
    buffer& bound = bound_[column - 1];
    std::memcpy(static_cast<char*>(bound.data) + row * bound.capacity,
      text.data(), text.size());
    bound.indicator[row] = static_cast<std::int64_t>(text.size());
  }

  void put_null(std::size_t column, std::size_t row) {
    bound_[column - 1].indicator[row] = backend::null_indicator;
  }

  // Stands in for a value too long for the buffer it was bound into.
  void put_overflow(std::size_t column, std::size_t row, std::string text) {
    long_text_[column - 1] = std::move(text);
    bound_[column - 1].indicator[row] = backend::no_total;
  }

  std::string read_long_text(std::size_t column) override {
    return long_text_[column - 1];
  }

  std::vector<column_info> column_meta() const override {
    return meta_;
  }

  void prepare(std::string_view) override {}
  void bind_parameter(std::size_t, sql_value const&) override {}
  void bind_batch_params(std::vector<params> const&) override {}
  std::vector<std::byte> read_long_bytes(std::size_t) override {
    return {};
  }
  backend::batch_writer_iface& prepare_batch() override {
    throw std::logic_error("unused by the read path");
  }
  void reset_parameters() override {}
  void execute() override {}
  bool fetch() override {
    return false;
  }
  std::size_t affected_rows() const override {
    return 0;
  }
  void set_row_array_size(std::size_t) override {}
  std::size_t rows_fetched() const override {
    return 0;
  }
  void set_paramset_size(std::size_t) override {}
  void reset() override {}

private:
  std::vector<column_info> meta_;
  std::vector<std::string> long_text_;
  std::vector<buffer> bound_;
};

}  // namespace

namespace uniorm {

template <>
struct converter<status> {
  using sql = std::string;

  static void to_db(status const& s, std::string& out) {
    if (s == status::paid)
      out = "paid";
    else if (s == status::shipped)
      out = "shipped";
    else
      out = "fresh";
  }

  static status from_db(std::string const& v) {
    if (v == "paid")
      return status::paid;
    if (v == "shipped")
      return status::shipped;
    return status::fresh;
  }
};

template <>
struct converter<money> {
  using sql = std::string;

  static void to_db(money const& m, std::string& out) {
    out = std::to_string(m.units) + "." + std::to_string(m.micros);
    out.resize(16, '0');  // past the short-string buffer on purpose
  }

  static money from_db(std::string const&) {
    return {};
  }
};

template <>
struct converter<tagged> {
  using sql = std::int16_t;

  static void to_db(tagged const& t, std::int16_t& out) {
    out = static_cast<std::int16_t>(t.code);
  }

  static tagged from_db(std::int16_t v) {
    return tagged{ static_cast<int>(v) };
  }
};

template <>
struct converter<half_wired> {
  using sql = std::string;

  static void to_db(half_wired const&, std::string& out) {
    out = "incomplete";
  }
};

}  // namespace uniorm

void test_converter_projection();

void test_converter() {
  CHECK(has_converter<status>);
  CHECK(has_converter<money>);
  CHECK(!has_converter<int>);
  CHECK(!has_converter<std::string>);
  CHECK(!has_converter<plain_enum>);
  CHECK(!has_converter<half_wired>);

  // The representation the converter names, not the integer the enum would
  // otherwise decay to.
  params p(status::paid, money{ 1234567, 890 });
  CHECK(std::get<std::string>(p.at(0)) == "paid");
  CHECK(std::get<std::string>(p.at(1)).size() == 16);
  CHECK(std::get<std::string>(p.at(1)).rfind("1234567.", 0) == 0);

  // An adapter beats an implicit conversion to std::string.
  params q(tagged{ 7 });
  CHECK(std::holds_alternative<std::int16_t>(q.at(0)));
  CHECK(std::get<std::int16_t>(q.at(0)) == 7);

  // Unconverted enums keep their existing path.
  params r(plain_enum::three);
  CHECK(std::get<std::int32_t>(r.at(0)) == 3);

  // Query predicates share the same encoding.
  std::vector<sql_value> bound;
  auto sql = eq(&record::state, status::shipped)
               .to_sql([](member_key const&) { return std::string("state"); },
                 bound);
  CHECK(sql == "state = ?");
  CHECK(bound.size() == 1);
  CHECK(std::get<std::string>(bound[0]) == "shipped");

  test_converter_projection();
}

void test_converter_projection() {
  fake_statement stmt({
    { "id", sql_type::bigint, 8, false },
    { "state", sql_type::varchar, 16, true },
    { "note", sql_type::varchar, 16, true },
  });
  detail::projection<read_row> proj;
  proj.set_row_array_size(2);
  proj.bind(stmt);

  // The representation picks the C buffer, not the domain type: an enum with
  // no adapter cannot be bound here at all, and the slot keeps the tight
  // stride the declared width gives a plain string column.
  CHECK(stmt.kind(2) == backend::buffer_type::chars);
  CHECK(stmt.stride(2) == 16 * 4 + 1);

  stmt.put(1, 0, std::int64_t{ 7 });
  stmt.put_text(2, 0, "paid");
  stmt.put_text(3, 0, "shipped");
  stmt.put(1, 1, std::int64_t{ 8 });
  stmt.put_text(2, 1, "fresh");
  stmt.put_null(3, 1);

  read_row out{};
  proj.fill_into(out, 0);
  CHECK(out.id == 7);
  CHECK(out.state == status::paid);
  CHECK(out.note && *out.note == status::shipped);

  // A NULL missed here would fill the optional with the decode of an absent
  // value, which is a domain value that was never in the column.
  proj.fill_into(out, 1);
  CHECK(out.state == status::fresh);
  CHECK(!out.note.has_value());

  stmt.put_null(2, 0);
  CHECK_THROWS(proj.fill_into(out, 0), type_mismatch);

  fake_statement narrow({ { "tag", sql_type::smallint, 6, false } });
  detail::projection<code_row> narrow_proj;
  narrow_proj.set_row_array_size(1);
  narrow_proj.bind(narrow);
  CHECK(narrow.kind(1) == backend::buffer_type::int16);
  narrow.put(1, 0, std::int16_t{ 7 });
  code_row codes{};
  narrow_proj.fill_into(codes, 0);
  CHECK(codes.tag.code == 7);

  // A value that overran its slot arrives through the continuation read,
  // and the decoding has to see that one rather than the truncated buffer.
  fake_statement tight({
    { "id", sql_type::bigint, 8, false },
    { "state", sql_type::varchar, 4, true },
    { "note", sql_type::varchar, 4, true },
  });
  detail::projection<read_row> tight_proj;
  tight_proj.set_row_array_size(1);
  tight_proj.bind(tight);
  tight.put(1, 0, std::int64_t{ 9 });
  tight.put_overflow(2, 0, "shipped");
  tight.put_overflow(3, 0, "paid");
  read_row wide{};
  tight_proj.fill_into(wide, 0);
  CHECK(wide.state == status::shipped);
  CHECK(wide.note && *wide.note == status::paid);
}
