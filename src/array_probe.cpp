#include "array_probe.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <uniorm/backend/error.hpp>
#include <uniorm/dialect.hpp>

namespace uniorm::detail {
namespace {

// The oracle/none families have no session-scoped temporary the probe can rely
// on, so their scratch is a permanent table no concurrent probe may share:
// the per-process stamp keeps another process -- or the crash residue of one
// -- from owning this name.
std::string unique_table_name() {
  static std::atomic<std::size_t> counter{ 0 };
  static std::size_t const process_stamp = [] {
    std::random_device seed;
    return seed() % 1000000;
  }();
  return "uniorm_probe_" + std::to_string(process_stamp) + "_" +
         std::to_string(counter.fetch_add(1));
}

struct scratch_table {
  std::string name;
  std::string create_sql;
  std::string drop_sql;
};

scratch_table plan_scratch(dialect const& detected) {
  // INTEGER/VARCHAR are the ANSI spellings every family here accepts, and
  // binding int32 keeps the probe clear of Oracle's int64-as-text mapping.
  std::string const columns =
    " (id INTEGER NOT NULL PRIMARY KEY, val INTEGER, tag VARCHAR(64))";
  switch (detected.batch_write) {
  case dialect::batch_family::mysql:
  case dialect::batch_family::postgresql:
    return { "uniorm_array_probe",
      "CREATE TEMPORARY TABLE uniorm_array_probe" + columns,
      "DROP TABLE uniorm_array_probe" };
  case dialect::batch_family::sql_server:
    return { "#uniorm_array_probe",
      "CREATE TABLE #uniorm_array_probe" + columns,
      "DROP TABLE #uniorm_array_probe" };
  case dialect::batch_family::oracle:
  case dialect::batch_family::none:
    break;
  }
  std::string name = unique_table_name();
  return { name, "CREATE TABLE " + name + columns, "DROP TABLE " + name };
}

bool is_connection_state(std::string const& state) {
  return state.size() >= 2 && state[0] == '0' && state[1] == '8';
}

// A throw is connection-level when the SQLSTATE class is 08 or the connection
// no longer reports open; the ops a probe never reached cannot conclude.
bool connection_level(
  backend::connection_iface& conn, backend::backend_error const& error) {
  if (!conn.is_open()) {
    return true;
  }
  for (auto const& diagnostic : error.diagnostics()) {
    if (is_connection_state(diagnostic.state)) {
      return true;
    }
  }
  return false;
}

struct scratch_guard {
  backend::connection_iface& conn;
  std::string drop_sql;
  ~scratch_guard() {
    try {
      auto statement = conn.create_statement();
      statement->prepare(drop_sql);
      statement->execute();
    } catch (...) {
      // Best effort: a temporary table is session-scoped anyway, and a dead
      // connection cannot drop anything.
    }
  }
};

std::optional<std::int64_t> read_count(
  backend::connection_iface& conn, std::string const& table) {
  auto statement = conn.create_statement();
  statement->prepare("SELECT COUNT(*) FROM " + table);
  std::int64_t count = 0;
  std::int64_t indicator = 0;
  statement->bind_column(1, backend::column_buffer{ backend::buffer_type::int64,
                              &count, sizeof(count), &indicator });
  statement->execute();
  if (!statement->fetch() || indicator == backend::null_indicator) {
    return std::nullopt;
  }
  return count;
}

std::optional<std::int64_t> read_val(
  backend::connection_iface& conn, std::string const& table, std::int32_t id) {
  auto statement = conn.create_statement();
  statement->prepare("SELECT val FROM " + table + " WHERE id = ?");
  statement->bind_parameter(1, sql_value{ id });
  std::int64_t value = 0;
  std::int64_t indicator = 0;
  statement->bind_column(1, backend::column_buffer{ backend::buffer_type::int64,
                              &value, sizeof(value), &indicator });
  statement->execute();
  if (!statement->fetch() || indicator == backend::null_indicator) {
    return std::nullopt;
  }
  return value;
}

// Clears with a non-array DELETE (universally executable) then seeds one row at
// a time, so the seed never depends on the array path under test.
void reseed(backend::connection_iface& conn, std::string const& table,
  std::size_t count) {
  auto clear = conn.create_statement();
  clear->prepare("DELETE FROM " + table);
  clear->execute();
  auto insert = conn.create_statement();
  insert->prepare("INSERT INTO " + table + " (id, val, tag) VALUES (?, ?, ?)");
  for (std::size_t index = 0; index < count; ++index) {
    std::vector<params> one{ params{ static_cast<std::int32_t>(index),
      static_cast<std::int32_t>(100 + index), "t" + std::to_string(index) } };
    insert->bind_batch_params(one);
    insert->set_paramset_size(1);
    insert->execute();
    insert->reset();
  }
}

template <class Verify>
probe_outcome run_op(backend::connection_iface& conn, std::string const& sql,
  std::vector<params> const& rows, Verify verify, bool& connection_lost,
  std::size_t* affected_out) {
  try {
    auto statement = conn.create_statement();
    statement->prepare(sql);
    statement->bind_batch_params(rows);
    statement->set_paramset_size(rows.size());
    statement->execute();
    if (affected_out != nullptr) {
      *affected_out = statement->affected_rows();
    }
  } catch (backend::backend_error const& error) {
    if (connection_level(conn, error)) {
      connection_lost = true;
      return probe_outcome::inconclusive;
    }
    return probe_outcome::fail;
  } catch (...) {
    connection_lost = !conn.is_open();
    return probe_outcome::inconclusive;
  }
  // The execute succeeded; the effect still has to be read back, because a
  // driver can report success and drop every row.
  try {
    return verify();
  } catch (backend::backend_error const& error) {
    if (connection_level(conn, error)) {
      connection_lost = true;
    }
    return probe_outcome::inconclusive;
  } catch (...) {
    connection_lost = !conn.is_open();
    return probe_outcome::inconclusive;
  }
}

template <class Verify>
probe_outcome run_seeded_op(backend::connection_iface& conn,
  std::string const& table, std::size_t count, std::string const& sql,
  std::vector<params> const& rows, Verify verify, bool& connection_lost,
  std::size_t* affected_out) {
  try {
    reseed(conn, table, count);
  } catch (backend::backend_error const& error) {
    if (connection_level(conn, error)) {
      connection_lost = true;
    }
    return probe_outcome::inconclusive;
  } catch (...) {
    connection_lost = !conn.is_open();
    return probe_outcome::inconclusive;
  }
  return run_op(conn, sql, rows, verify, connection_lost, affected_out);
}

}  // namespace

array_probe_result probe_array_dml(
  backend::connection_iface& conn, std::size_t paramset_size) {
  array_probe_result result;
  if (paramset_size < 2) {
    paramset_size = 2;
  }
  std::size_t const count = paramset_size;

  scratch_table scratch;
  try {
    dialect const detected = dialect::detect(conn.dbms_name());
    scratch = plan_scratch(detected);
    conn.set_autocommit(true);
    auto statement = conn.create_statement();
    statement->prepare(scratch.create_sql);
    statement->execute();
  } catch (...) {
    return result;
  }
  scratch_guard guard{ conn, scratch.drop_sql };

  std::string const& table = scratch.name;
  bool connection_lost = false;

  std::vector<params> insert_rows;
  for (std::size_t index = 0; index < count; ++index) {
    insert_rows.emplace_back(static_cast<std::int32_t>(index),
      static_cast<std::int32_t>(100 + index), "t" + std::to_string(index));
  }
  result.insert = run_op(
    conn, "INSERT INTO " + table + " (id, val, tag) VALUES (?, ?, ?)",
    insert_rows,
    [&]() -> probe_outcome {
      auto rows = read_count(conn, table);
      if (!rows || *rows != static_cast<std::int64_t>(count)) {
        return rows ? probe_outcome::fail : probe_outcome::inconclusive;
      }
      auto value = read_val(conn, table, 0);
      if (!value) {
        return probe_outcome::inconclusive;
      }
      return *value == 100 ? probe_outcome::pass : probe_outcome::fail;
    },
    connection_lost, nullptr);
  if (connection_lost) {
    return result;
  }

  std::vector<params> update_rows;
  for (std::size_t index = 0; index < count; ++index) {
    update_rows.emplace_back(static_cast<std::int32_t>(200 + index),
      "u" + std::to_string(index), static_cast<std::int32_t>(index));
  }
  std::size_t update_affected = 0;
  result.update = run_seeded_op(
    conn, table, count,
    "UPDATE " + table + " SET val = ?, tag = ? WHERE id = ?", update_rows,
    [&]() -> probe_outcome {
      auto rows = read_count(conn, table);
      if (!rows || *rows != static_cast<std::int64_t>(count)) {
        return rows ? probe_outcome::fail : probe_outcome::inconclusive;
      }
      auto value = read_val(conn, table, 0);
      if (!value) {
        return probe_outcome::inconclusive;
      }
      return *value == 200 ? probe_outcome::pass : probe_outcome::fail;
    },
    connection_lost, &update_affected);
  if (result.update == probe_outcome::pass) {
    result.rowcount_totals = update_affected == count;
  }
  if (connection_lost) {
    return result;
  }

  std::vector<params> delete_rows;
  for (std::size_t index = 0; index < count; ++index) {
    delete_rows.emplace_back(static_cast<std::int32_t>(index));
  }
  result.remove = run_seeded_op(
    conn, table, count, "DELETE FROM " + table + " WHERE id = ?", delete_rows,
    [&]() -> probe_outcome {
      auto rows = read_count(conn, table);
      if (!rows) {
        return probe_outcome::inconclusive;
      }
      return *rows == 0 ? probe_outcome::pass : probe_outcome::fail;
    },
    connection_lost, nullptr);
  return result;
}

}  // namespace uniorm::detail