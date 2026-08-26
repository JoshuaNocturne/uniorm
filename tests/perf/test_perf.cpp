// Performance benchmarks against a live database via ODBC DSN.
// DSN, user and password come from UNIORM_IT_DSN / UNIORM_IT_USER /
// UNIORM_IT_PWD; row count from UNIORM_PERF_ROWS (default: 10000).
// Returns 77 (ctest SKIP) when any of them is unset or the database is
// unreachable. Compares the query materialization paths:
// entity direct binding, aggregate projection, and dynamic rows, plus a
// raw ODBC baseline using SQL_ATTR_PARAMSET_SIZE for batch insert/update/
// delete and SQL_ATTR_ROW_ARRAY_SIZE for block fetching.

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <sql.h>
#include <sqlext.h>

#include <uniorm/detail/connection.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/query/builder.hpp>

using namespace uniorm;
using perf_clock = std::chrono::steady_clock;

namespace uniorm {

struct Bench {
  std::int64_t id = 0;
  std::string name;
  std::int32_t score = 0;
  std::optional<std::string> note;
};

}  // namespace uniorm

namespace {

char const* k_table = "uniorm_perf_bench";

orm build_registry(std::string const& conn_string) {
  orm registry(conn_string);
  registry.map<Bench>(k_table)
    .primary_key("id", &Bench::id)
    .column("name", &Bench::name)
    .column("score", &Bench::score)
    .column("note", &Bench::note);
  return registry;
}

void prepare_schema(connection& conn) {
  conn.execute_update(std::string("DROP TABLE IF EXISTS ") + k_table);
  conn.execute_update(std::string("CREATE TABLE ") + k_table +
                      " (id BIGINT NOT NULL PRIMARY KEY,"
                      " name VARCHAR(64) NOT NULL,"
                      " score INT NOT NULL,"
                      " note VARCHAR(64) NULL)");
}

std::vector<Bench> make_rows(std::size_t n) {
  std::vector<Bench> rows;
  rows.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    Bench b;
    b.id = static_cast<std::int64_t>(i);
    b.name = "row-" + std::to_string(i);
    b.score = static_cast<std::int32_t>(i % 1000);
    if (i % 4 != 0) {
      b.note = "note-" + std::to_string(i);
    }
    rows.push_back(std::move(b));
  }
  return rows;
}

void report(
  char const* name, std::size_t rows, perf_clock::duration elapsed, int runs) {
  double ms = std::chrono::duration<double, std::milli>(elapsed).count();
  double per_run_rows = static_cast<double>(rows) / runs;
  double krows_per_s = per_run_rows / ms;  // rows/ms == krows/s
  std::printf("%-34s %10.2f ms %12.1f krows/s\n", name, ms, krows_per_s);
}

template <class Fn>
perf_clock::duration best_of(Fn&& fn, int runs) {
  perf_clock::duration best = perf_clock::duration::max();
  for (int i = 0; i < runs; ++i) {
    auto start = perf_clock::now();
    fn();
    auto elapsed = perf_clock::now() - start;
    if (elapsed < best) {
      best = elapsed;
    }
  }
  return best;
}

void run_benchmarks(connection& conn, orm& registry, std::size_t n) {
  int const runs = 3;
  std::printf("rows per case: %zu (best of %d runs)\n", n, runs);
  std::printf("\n[uniorm]\n");
  std::printf("%-34s %12s %14s\n", "benchmark", "time", "throughput");

  std::vector<Bench> rows = make_rows(n);
  std::size_t inserted = 0;
  {
    perf_clock::duration best = perf_clock::duration::max();
    for (int i = 0; i < runs; ++i) {
      conn.execute_update(std::string("DELETE FROM ") + k_table);
      auto start = perf_clock::now();
      inserted = registry.insert(rows);
      auto elapsed = perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    report("insert (batch)", n, best, 1);
  }
  if (inserted != n) {
    std::printf("FATAL: inserted %zu of %zu rows\n", inserted, n);
    std::exit(1);
  }

  // Batch update benchmark: update score column for all rows
  std::size_t updated = 0;
  {
    perf_clock::duration best = perf_clock::duration::max();
    for (int i = 0; i < runs; ++i) {
      // Reset scores to original values before each run
      if (i > 0) {
        std::vector<Bench> reset_rows = rows;
        registry.update(reset_rows);
      }
      // Prepare updated rows: increment score by 1
      std::vector<Bench> update_rows = rows;
      for (auto& b : update_rows) {
        b.score += 1;
      }
      auto start = perf_clock::now();
      updated = registry.update(update_rows);
      auto elapsed = perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    report("update (batch)", n, best, 1);
  }
  if (updated != n) {
    std::printf("FATAL: updated %zu of %zu rows\n", updated, n);
    std::exit(1);
  }

  // Batch delete benchmark: delete half the rows
  std::size_t deleted = 0;
  {
    // Prepare keys for deletion: delete rows with even IDs
    std::vector<params> delete_keys;
    for (std::size_t i = 0; i < n; i += 2) {
      delete_keys.emplace_back(std::vector<sql_value>{static_cast<std::int64_t>(i)});
    }
    perf_clock::duration best = perf_clock::duration::max();
    for (int i = 0; i < runs; ++i) {
      // Re-insert deleted rows before each run
      if (i > 0) {
        std::vector<Bench> reinsert_rows;
        for (std::size_t j = 0; j < n; j += 2) {
          Bench b;
          b.id = static_cast<std::int64_t>(j);
          b.name = "row-" + std::to_string(j);
          b.score = static_cast<std::int32_t>(j % 1000);
          if (j % 4 != 0) {
            b.note = "note-" + std::to_string(j);
          }
          reinsert_rows.push_back(std::move(b));
        }
        registry.insert(reinsert_rows);
      }
      auto start = perf_clock::now();
      deleted = conn.remove_batch(k_table, {"id"}, delete_keys);
      auto elapsed = perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    report("delete (batch)", delete_keys.size(), best, 1);
  }
  std::size_t expected_deleted = (n + 1) / 2;  // ceiling division
  if (deleted != expected_deleted) {
    std::printf("FATAL: deleted %zu of %zu rows\n", deleted, expected_deleted);
    std::exit(1);
  }

  // Re-insert deleted rows for query benchmarks
  {
    std::vector<Bench> reinsert_rows;
    for (std::size_t i = 0; i < n; i += 2) {
      Bench b;
      b.id = static_cast<std::int64_t>(i);
      b.name = "row-" + std::to_string(i);
      b.score = static_cast<std::int32_t>(i % 1000);
      if (i % 4 != 0) {
        b.note = "note-" + std::to_string(i);
      }
      reinsert_rows.push_back(std::move(b));
    }
    registry.insert(reinsert_rows);
  }

  std::string select_all =
    std::string("SELECT id, name, score, note FROM ") + k_table;

  std::size_t entity_rows = 0;
  report("query entity all() (direct bind)", n,
    best_of(
      [&] { entity_rows = registry.query().of<Bench>().all().size(); },
      runs),
    runs);
  if (entity_rows != n) {
    std::printf("FATAL: entity query returned %zu rows\n", entity_rows);
    std::exit(1);
  }

  struct bench_row {
    std::int64_t id;
    std::string name;
    std::int32_t score;
    std::optional<std::string> note;
  };
  std::size_t proj_rows = 0;
  report("query aggregate projection", n,
    best_of(
      [&] { proj_rows = conn.query<bench_row>(select_all).size(); }, runs),
    runs);
  if (proj_rows != n) {
    std::printf("FATAL: projection returned %zu rows\n", proj_rows);
    std::exit(1);
  }

  std::size_t dynamic_rows = 0;
  std::int64_t id_sum = 0;
  report("query dynamic rows (row/sql_value)", n,
    best_of(
      [&] {
        result_set rs = conn.execute(select_all);
        dynamic_rows = 0;
        while (rs.next()) {
          row r = rs.current();
          id_sum += r.get<std::int64_t>("id");
          ++dynamic_rows;
        }
      },
      runs),
    runs);
  if (dynamic_rows != n) {
    std::printf("FATAL: dynamic query returned %zu rows\n", dynamic_rows);
    std::exit(1);
  }

  report("query one() (direct bind)", 1,
    best_of(
      [&] {
        auto one = registry.query().of<Bench>().limit(1).one();
        if (!one) {
          std::exit(1);
        }
      },
      runs),
    runs);

  std::int64_t count = 0;
  report("query count()", 1,
    best_of([&] { count = registry.query().of<Bench>().count(); }, runs),
    runs);
  if (count != static_cast<std::int64_t>(n)) {
    std::printf("FATAL: count() returned %" PRId64 "\n", count);
    std::exit(1);
  }
  (void)id_sum;
}

// ---- raw ODBC baseline: same ODBC call patterns as uniorm -----------

[[noreturn]] void odbc_fail(
  SQLSMALLINT handle_type, SQLHANDLE handle, char const* context) {
  SQLCHAR state[16] = {};
  SQLCHAR text[512] = {};
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  SQLGetDiagRec(
    handle_type, handle, 1, state, &native, text, sizeof(text), &len);
  std::printf("FATAL: raw ODBC %s: [%s] %s\n", context, state, text);
  std::exit(1);
}

void odbc_check(SQLRETURN rc, SQLSMALLINT handle_type, SQLHANDLE handle,
  char const* context) {
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    odbc_fail(handle_type, handle, context);
  }
}

struct raw_connection {
  SQLHENV env = SQL_NULL_HANDLE;
  SQLHDBC dbc = SQL_NULL_HANDLE;

  explicit raw_connection(std::string const& conn_string) {
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
    SQLCHAR out[1024];
    SQLSMALLINT out_len = 0;
    odbc_check(
      SQLDriverConnect(dbc, nullptr,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(conn_string.c_str())),
        SQL_NTS, out, sizeof(out), &out_len, SQL_DRIVER_NOPROMPT),
      SQL_HANDLE_DBC, dbc, "connect");
  }

  ~raw_connection() {
    if (dbc != SQL_NULL_HANDLE) {
      SQLDisconnect(dbc);
      SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    }
    if (env != SQL_NULL_HANDLE) {
      SQLFreeHandle(SQL_HANDLE_ENV, env);
    }
  }
};

struct raw_statement {
  SQLHSTMT stmt = SQL_NULL_HANDLE;

  raw_statement(SQLHDBC dbc, std::string const& sql) {
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
    odbc_check(
      SQLPrepare(stmt,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS),
      SQL_HANDLE_STMT, stmt, "prepare");
  }

  void execute() {
    odbc_check(SQLExecute(stmt), SQL_HANDLE_STMT, stmt, "execute");
  }

  ~raw_statement() {
    if (stmt != SQL_NULL_HANDLE) {
      SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }
  }
};

void run_raw_benchmarks(std::string const& conn_string, std::size_t n) {
  int const runs = 3;
  std::printf("\n[raw ODBC baseline]\n");
  std::printf("%-34s %12s %14s\n", "benchmark", "time", "throughput");

  raw_connection rc(conn_string);
  std::size_t const batch_size = 1000;

  // --- Batch insert using SQL_ATTR_PARAMSET_SIZE ---
  // Prepare a single-row INSERT, bind column-wise arrays, set paramset size.
  {
    raw_statement del(rc.dbc, std::string("DELETE FROM ") + k_table);
    odbc_check(SQLExecute(del.stmt), SQL_HANDLE_STMT, del.stmt,
      "delete before raw batch insert");
  }

  std::size_t batch_inserted = 0;
  report("insert (paramset)", n,
    best_of(
      [&] {
        SQLSetConnectAttr(
          rc.dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);

        std::vector<SQLBIGINT> ids(batch_size);
        std::vector<char> names(batch_size * 65);
        std::vector<SQLINTEGER> scores(batch_size);
        std::vector<char> notes(batch_size * 65);
        std::vector<SQLLEN> name_inds(batch_size);
        std::vector<SQLLEN> note_inds(batch_size);

        raw_statement ins(rc.dbc,
          std::string("INSERT INTO ") + k_table + " (id, name, score, note) "
          "VALUES (?, ?, ?, ?)");

        SQLULEN paramset = batch_size;
        odbc_check(SQLSetStmtAttr(ins.stmt, SQL_ATTR_PARAMSET_SIZE,
                       reinterpret_cast<SQLPOINTER>(paramset), 0),
          SQL_HANDLE_STMT, ins.stmt, "set paramset size");

        odbc_check(SQLBindParameter(ins.stmt, 1, SQL_PARAM_INPUT,
                         SQL_C_SBIGINT, SQL_BIGINT, 0, 0, ids.data(),
                         sizeof(SQLBIGINT), nullptr),
          SQL_HANDLE_STMT, ins.stmt, "bind paramset id");
        odbc_check(SQLBindParameter(ins.stmt, 2, SQL_PARAM_INPUT,
                         SQL_C_CHAR, SQL_VARCHAR, 64, 0, names.data(),
                         65, name_inds.data()),
          SQL_HANDLE_STMT, ins.stmt, "bind paramset name");
        odbc_check(SQLBindParameter(ins.stmt, 3, SQL_PARAM_INPUT,
                         SQL_C_SLONG, SQL_INTEGER, 0, 0, scores.data(),
                         sizeof(SQLINTEGER), nullptr),
          SQL_HANDLE_STMT, ins.stmt, "bind paramset score");
        odbc_check(SQLBindParameter(ins.stmt, 4, SQL_PARAM_INPUT,
                         SQL_C_CHAR, SQL_VARCHAR, 64, 0, notes.data(),
                         65, note_inds.data()),
          SQL_HANDLE_STMT, ins.stmt, "bind paramset note");

        batch_inserted = 0;
        for (std::size_t start = 0; start < n; start += batch_size) {
          std::size_t count = std::min(batch_size, n - start);
          if (count != batch_size) {
            paramset = count;
            odbc_check(SQLSetStmtAttr(ins.stmt, SQL_ATTR_PARAMSET_SIZE,
                           reinterpret_cast<SQLPOINTER>(paramset), 0),
              SQL_HANDLE_STMT, ins.stmt, "set paramset size (tail)");
          }
          for (std::size_t i = 0; i < count; ++i) {
            std::size_t row = start + i;
            ids[i] = static_cast<SQLBIGINT>(row);
            std::snprintf(&names[i * 65], 65, "row-%zu", row);
            scores[i] = static_cast<SQLINTEGER>(row % 1000);
            if (row % 4 == 0) {
              note_inds[i] = SQL_NULL_DATA;
            } else {
              std::snprintf(&notes[i * 65], 65, "note-%zu", row);
              note_inds[i] = SQL_NTS;
            }
          }
          odbc_check(SQLExecute(ins.stmt), SQL_HANDLE_STMT, ins.stmt,
            "execute paramset insert");
          batch_inserted += count;
        }
        odbc_check(SQLEndTran(SQL_HANDLE_DBC, rc.dbc, SQL_COMMIT),
          SQL_HANDLE_DBC, rc.dbc, "commit raw batch insert");
        SQLSetConnectAttr(
          rc.dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);
      },
      1),
    1);
  if (batch_inserted != n) {
    std::printf(
      "FATAL: raw batch inserted %zu of %zu rows\n", batch_inserted, n);
    std::exit(1);
  }

  // --- Batch update using SQL_ATTR_PARAMSET_SIZE ---
  std::size_t batch_updated = 0;
  report("update (paramset)", n,
    best_of(
      [&] {
        std::vector<SQLINTEGER> scores(batch_size);
        std::vector<SQLBIGINT> ids(batch_size);

        raw_statement upd(rc.dbc,
          std::string("UPDATE ") + k_table + " SET score = ? WHERE id = ?");

        SQLULEN paramset = batch_size;
        odbc_check(SQLSetStmtAttr(upd.stmt, SQL_ATTR_PARAMSET_SIZE,
                       reinterpret_cast<SQLPOINTER>(paramset), 0),
          SQL_HANDLE_STMT, upd.stmt, "set paramset size");

        odbc_check(SQLBindParameter(upd.stmt, 1, SQL_PARAM_INPUT,
                         SQL_C_SLONG, SQL_INTEGER, 0, 0, scores.data(),
                         sizeof(SQLINTEGER), nullptr),
          SQL_HANDLE_STMT, upd.stmt, "bind paramset score");
        odbc_check(SQLBindParameter(upd.stmt, 2, SQL_PARAM_INPUT,
                         SQL_C_SBIGINT, SQL_BIGINT, 0, 0, ids.data(),
                         sizeof(SQLBIGINT), nullptr),
          SQL_HANDLE_STMT, upd.stmt, "bind paramset id");

        batch_updated = 0;
        for (std::size_t start = 0; start < n; start += batch_size) {
          std::size_t count = std::min(batch_size, n - start);
          if (count != batch_size) {
            paramset = count;
            odbc_check(SQLSetStmtAttr(upd.stmt, SQL_ATTR_PARAMSET_SIZE,
                           reinterpret_cast<SQLPOINTER>(paramset), 0),
              SQL_HANDLE_STMT, upd.stmt, "set paramset size (tail)");
          }
          for (std::size_t i = 0; i < count; ++i) {
            ids[i] = static_cast<SQLBIGINT>(start + i);
            scores[i] = static_cast<SQLINTEGER>((start + i) % 1000 + 1);
          }
          odbc_check(SQLExecute(upd.stmt), SQL_HANDLE_STMT, upd.stmt,
            "execute paramset update");
          batch_updated += count;
        }
      },
      runs),
    runs);
  if (batch_updated != n) {
    std::printf("FATAL: raw batch updated %zu of %zu rows\n", batch_updated, n);
    std::exit(1);
  }

  // --- Batch delete using SQL_ATTR_PARAMSET_SIZE ---
  std::size_t batch_deleted = 0;
  report("delete (paramset)", n,
    best_of(
      [&] {
        std::vector<SQLBIGINT> ids(batch_size);

        raw_statement del(rc.dbc,
          std::string("DELETE FROM ") + k_table + " WHERE id = ?");

        SQLULEN paramset = batch_size;
        odbc_check(SQLSetStmtAttr(del.stmt, SQL_ATTR_PARAMSET_SIZE,
                       reinterpret_cast<SQLPOINTER>(paramset), 0),
          SQL_HANDLE_STMT, del.stmt, "set paramset size");

        odbc_check(SQLBindParameter(del.stmt, 1, SQL_PARAM_INPUT,
                         SQL_C_SBIGINT, SQL_BIGINT, 0, 0, ids.data(),
                         sizeof(SQLBIGINT), nullptr),
          SQL_HANDLE_STMT, del.stmt, "bind paramset id");

        batch_deleted = 0;
        for (std::size_t start = 0; start < n; start += batch_size) {
          std::size_t count = std::min(batch_size, n - start);
          if (count != batch_size) {
            paramset = count;
            odbc_check(SQLSetStmtAttr(del.stmt, SQL_ATTR_PARAMSET_SIZE,
                           reinterpret_cast<SQLPOINTER>(paramset), 0),
              SQL_HANDLE_STMT, del.stmt, "set paramset size (tail)");
          }
          for (std::size_t i = 0; i < count; ++i) {
            ids[i] = static_cast<SQLBIGINT>(start + i);
          }
          odbc_check(SQLExecute(del.stmt), SQL_HANDLE_STMT, del.stmt,
            "execute paramset delete");
          batch_deleted += count;
        }
      },
      runs),
    runs);
  if (batch_deleted != n) {
    std::printf("FATAL: raw batch deleted %zu of %zu rows\n", batch_deleted, n);
    std::exit(1);
  }

  // Re-insert data for query benchmarks
  {
    raw_statement ins(rc.dbc,
      std::string("INSERT INTO ") + k_table + " (id, name, score, note) "
      "VALUES (?, ?, ?, ?)");
    SQLULEN paramset = batch_size;
    odbc_check(SQLSetStmtAttr(ins.stmt, SQL_ATTR_PARAMSET_SIZE,
                   reinterpret_cast<SQLPOINTER>(paramset), 0),
      SQL_HANDLE_STMT, ins.stmt, "set paramset size");

    std::vector<SQLBIGINT> ids(batch_size);
    std::vector<char> names(batch_size * 65);
    std::vector<SQLINTEGER> scores(batch_size);
    std::vector<char> notes(batch_size * 65);
    std::vector<SQLLEN> name_inds(batch_size);
    std::vector<SQLLEN> note_inds(batch_size);

    odbc_check(SQLBindParameter(ins.stmt, 1, SQL_PARAM_INPUT,
                     SQL_C_SBIGINT, SQL_BIGINT, 0, 0, ids.data(),
                     sizeof(SQLBIGINT), nullptr),
      SQL_HANDLE_STMT, ins.stmt, "bind paramset id");
    odbc_check(SQLBindParameter(ins.stmt, 2, SQL_PARAM_INPUT,
                     SQL_C_CHAR, SQL_VARCHAR, 64, 0, names.data(),
                     65, name_inds.data()),
      SQL_HANDLE_STMT, ins.stmt, "bind paramset name");
    odbc_check(SQLBindParameter(ins.stmt, 3, SQL_PARAM_INPUT,
                     SQL_C_SLONG, SQL_INTEGER, 0, 0, scores.data(),
                     sizeof(SQLINTEGER), nullptr),
      SQL_HANDLE_STMT, ins.stmt, "bind paramset score");
    odbc_check(SQLBindParameter(ins.stmt, 4, SQL_PARAM_INPUT,
                     SQL_C_CHAR, SQL_VARCHAR, 64, 0, notes.data(),
                     65, note_inds.data()),
      SQL_HANDLE_STMT, ins.stmt, "bind paramset note");

    for (std::size_t start = 0; start < n; start += batch_size) {
      std::size_t count = std::min(batch_size, n - start);
      if (count != batch_size) {
        paramset = count;
        odbc_check(SQLSetStmtAttr(ins.stmt, SQL_ATTR_PARAMSET_SIZE,
                       reinterpret_cast<SQLPOINTER>(paramset), 0),
          SQL_HANDLE_STMT, ins.stmt, "set paramset size (tail)");
      }
      for (std::size_t i = 0; i < count; ++i) {
        std::size_t row = start + i;
        ids[i] = static_cast<SQLBIGINT>(row);
        std::snprintf(&names[i * 65], 65, "row-%zu", row);
        scores[i] = static_cast<SQLINTEGER>(row % 1000);
        if (row % 4 == 0) {
          note_inds[i] = SQL_NULL_DATA;
        } else {
          std::snprintf(&notes[i * 65], 65, "note-%zu", row);
          note_inds[i] = SQL_NTS;
        }
      }
      odbc_check(SQLExecute(ins.stmt), SQL_HANDLE_STMT, ins.stmt,
        "execute paramset insert");
    }
  }

  std::string select_all =
    std::string("SELECT id, name, score, note FROM ") + k_table;

  // --- Block fetch using SQL_ATTR_ROW_ARRAY_SIZE ---
  std::size_t const fetch_size = 100;
  std::size_t block_rows = 0;
  report("query (block fetch)", n,
    best_of(
      [&] {
        raw_statement q(rc.dbc, select_all);

        std::vector<SQLBIGINT> ids(fetch_size);
        std::vector<char> names(fetch_size * 65);
        std::vector<SQLINTEGER> scores(fetch_size);
        std::vector<char> notes(fetch_size * 65);
        std::vector<SQLLEN> id_inds(fetch_size);
        std::vector<SQLLEN> name_inds(fetch_size);
        std::vector<SQLLEN> score_inds(fetch_size);
        std::vector<SQLLEN> note_inds(fetch_size);
        SQLULEN rows_fetched = 0;

        odbc_check(SQLSetStmtAttr(q.stmt, SQL_ATTR_ROW_ARRAY_SIZE,
                       reinterpret_cast<SQLPOINTER>(fetch_size), 0),
          SQL_HANDLE_STMT, q.stmt, "set row array size");
        odbc_check(SQLSetStmtAttr(q.stmt, SQL_ATTR_ROWS_FETCHED_PTR,
                       &rows_fetched, 0),
          SQL_HANDLE_STMT, q.stmt, "set rows fetched ptr");

        odbc_check(SQLBindCol(q.stmt, 1, SQL_C_SBIGINT, ids.data(),
                       sizeof(SQLBIGINT), id_inds.data()),
          SQL_HANDLE_STMT, q.stmt, "bindcol 1");
        odbc_check(SQLBindCol(q.stmt, 2, SQL_C_CHAR, names.data(),
                       65, name_inds.data()),
          SQL_HANDLE_STMT, q.stmt, "bindcol 2");
        odbc_check(SQLBindCol(q.stmt, 3, SQL_C_SLONG, scores.data(),
                       sizeof(SQLINTEGER), score_inds.data()),
          SQL_HANDLE_STMT, q.stmt, "bindcol 3");
        odbc_check(SQLBindCol(q.stmt, 4, SQL_C_CHAR, notes.data(),
                       65, note_inds.data()),
          SQL_HANDLE_STMT, q.stmt, "bindcol 4");

        block_rows = 0;
        q.execute();
        SQLRETURN fetch_rc = SQLFetch(q.stmt);
        while (fetch_rc == SQL_SUCCESS || fetch_rc == SQL_SUCCESS_WITH_INFO) {
          block_rows += rows_fetched;
          fetch_rc = SQLFetch(q.stmt);
        }
      },
      runs),
    runs);
  if (block_rows != n) {
    std::printf("FATAL: raw block fetch returned %zu rows\n", block_rows);
    std::exit(1);
  }

  // Single-row fetch, mirroring one() (LIMIT 1 + one SQLFetch).
  report("query single row (LIMIT 1)", 1,
    best_of(
      [&] {
        raw_statement q(rc.dbc, select_all + " LIMIT 1");
        SQLBIGINT id = 0;
        char name[65];
        SQLINTEGER score = 0;
        char note[65];
        SQLLEN id_ind = 0, name_ind = 0, score_ind = 0, note_ind = 0;
        SQLBindCol(q.stmt, 1, SQL_C_SBIGINT, &id, sizeof(id), &id_ind);
        SQLBindCol(q.stmt, 2, SQL_C_CHAR, name, sizeof(name), &name_ind);
        SQLBindCol(q.stmt, 3, SQL_C_SLONG, &score, sizeof(score), &score_ind);
        SQLBindCol(q.stmt, 4, SQL_C_CHAR, note, sizeof(note), &note_ind);
        q.execute();
        if (SQLFetch(q.stmt) != SQL_SUCCESS) {
          std::exit(1);
        }
      },
      runs),
    runs);
}

}  // namespace

int main() {
  char const* dsn_env = std::getenv("UNIORM_IT_DSN");
  char const* user_env = std::getenv("UNIORM_IT_USER");
  char const* pwd_env = std::getenv("UNIORM_IT_PWD");
  char const* rows_env = std::getenv("UNIORM_PERF_ROWS");
  if (!dsn_env || !*dsn_env || !user_env || !*user_env || !pwd_env ||
      !*pwd_env) {
    std::printf(
      "skip: set UNIORM_IT_DSN / UNIORM_IT_USER / UNIORM_IT_PWD to run "
      "perf tests\n");
    return 77;
  }
  std::string dsn = dsn_env;
  std::string user = user_env;
  std::string pwd = pwd_env;
  std::string conn_string = "DSN=" + dsn + ";UID=" + user + ";PWD=" + pwd;
  std::size_t n =
    rows_env && *rows_env ? std::strtoul(rows_env, nullptr, 10) : 10000;
  if (n == 0 || n > 1'000'000) {
    n = 10000;
  }

  try {
    connection probe(conn_string);
    std::printf("connected: dbms = %s\n", probe.dbms_name().c_str());
  } catch (std::exception const& e) {
    std::printf(
      "skip: cannot connect to DSN '%s': %s\n", dsn.c_str(), e.what());
    return 77;
  }

  try {
    connection conn(conn_string);
    prepare_schema(conn);
    orm registry = build_registry(conn_string);
    run_benchmarks(conn, registry, n);
    run_raw_benchmarks(conn_string, n);
    conn.execute_update(std::string("DROP TABLE ") + k_table);
  } catch (std::exception const& e) {
    std::printf("FATAL: unexpected exception: %s\n", e.what());
    return 1;
  }
  return 0;
}
