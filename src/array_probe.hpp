#pragma once

// Private header: not installed. The effect-level array-DML probe shared by
// the CI compatibility-table validation and the ODBC backend's runtime
// fallback. Backend-neutral: it drives only connection_iface/statement_iface,
// so it compiles and is fake-testable without any driver.

#include <cstddef>

#include <uniorm/backend/backend.hpp>
#include <uniorm/export.hpp>

namespace uniorm::detail {

// What one op's array execute proved about its effect on the rows.
enum class probe_outcome { pass, fail, inconclusive };

struct array_probe_result {
  probe_outcome insert = probe_outcome::inconclusive;
  probe_outcome update = probe_outcome::inconclusive;
  probe_outcome remove = probe_outcome::inconclusive;
  // Measured during UPDATE: an N-set execute reporting N affected rows tallies
  // the whole array; one reporting fewer (psqlODBC reports 1) does not. Left
  // true when UPDATE never ran to a measurable count.
  bool rowcount_totals = true;
};

// Runs PARAMSET_SIZE=paramset_size INSERT/UPDATE/DELETE on a scratch table and
// verifies the row state by SELECT, never trusting affected_rows() alone. A
// statement-level error fails that op; a connection-level error or a scratch
// DDL failure leaves the ops inconclusive. A small sample on three columns
// cannot prove every shape and parameter-set size safe; the integration suite
// at real entity shapes is the backstop.
UNIORM_API array_probe_result probe_array_dml(
  backend::connection_iface& conn, std::size_t paramset_size = 8);

}  // namespace uniorm::detail