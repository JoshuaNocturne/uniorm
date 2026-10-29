#include <cstdint>
#include <variant>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/error.hpp>

#include "libpq/connection.hpp"
#include "libpq/error.hpp"
#include "libpq/statement.hpp"

#include "check.hpp"

using namespace uniorm;

void test_libpq_statement_declines_a_batch_writer() {
  // A batch here is one execute per row, so the direct-write channel has
  // nothing behind it and says so rather than quietly buffering.
  libpq::connection conn;
  libpq::pq_statement stmt(conn);
  CHECK_THROWS(stmt.prepare_batch(), backend::capability_not_supported);
}

void test_libpq_statement_binds_nothing_before_a_prepare() {
  // A statement the server has never seen declares no parameters, so every
  // position the caller names is out of range, and every read of a result that
  // does not exist is answered with nothing rather than a guess.
  libpq::connection conn;
  libpq::pq_statement stmt(conn);
  sql_value const seven{std::in_place_type<std::int32_t>, 7};
  CHECK_THROWS(stmt.bind_parameter(0, seven), libpq::pq_error);
  CHECK_THROWS(stmt.bind_parameter(1, seven), libpq::pq_error);

  // The row-count hint this backend has no use for is ignored, not refused.
  stmt.set_paramset_size(10);
  stmt.set_row_array_size(4);

  CHECK(stmt.rows_fetched() == 0);
  CHECK(stmt.affected_rows() == 0);
  CHECK(stmt.result_row_estimate() == 0);
  CHECK(stmt.column_meta().empty());
  CHECK(!stmt.fetch());
  CHECK_THROWS(stmt.read_long_text(1, 0), libpq::pq_error);
  CHECK_THROWS(stmt.read_long_bytes(1, 0), libpq::pq_error);

  // Dropping what a reuse would otherwise leave behind works with no result to
  // drop, and a statement is handed back to the cache in that state.
  stmt.reset();
  CHECK(stmt.column_meta().empty());
}

void test_libpq_statement_refuses_a_column_before_a_result() {
  // A binding has nowhere to be recorded until the statement has run, and a
  // continuation read of a column nobody bound is this library's failure, not
  // a silent null.
  libpq::connection conn;
  libpq::pq_statement stmt(conn);
  backend::column_buffer const buffer{
    backend::buffer_type::chars, nullptr, 0, nullptr};
  CHECK_THROWS(stmt.bind_column(0, buffer), libpq::pq_error);
  CHECK_THROWS(stmt.bind_column(1, buffer), libpq::pq_error);
  CHECK_THROWS(stmt.read_long_text(1, 0), libpq::pq_error);
}
