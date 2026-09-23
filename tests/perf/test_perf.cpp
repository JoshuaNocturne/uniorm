// Performance benchmarks against a live database via ODBC DSN.
// DSN, user and password come from UNIORM_IT_DSN / UNIORM_IT_USER /
// UNIORM_IT_PWD; row count from UNIORM_PERF_ROWS (default: 10000).
// Returns 77 (ctest SKIP) when any of them is unset or the database is
// unreachable. Compares the query materialization paths:
// entity direct binding, aggregate projection, and dynamic rows, plus a
// raw ODBC baseline using SQL_ATTR_PARAMSET_SIZE for batch insert/update/
// delete and SQL_ATTR_ROW_ARRAY_SIZE for block fetching. The last three
// cases run the same bytes through a converter-backed field, so the cost
// of the extension point is measurable against the plain field beside it.

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

#include <uniorm/connection.hpp>
#include <uniorm/converter.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/builder/builder.hpp>

using namespace uniorm;
using perf_clock = std::chrono::steady_clock;

namespace uniorm {

struct Bench {
  std::int64_t id = 0;
  std::string name;
  std::int32_t score = 0;
  std::optional<std::string> note;
};

// A domain type whose representation is the column's own text. to_db copies
// because the entity it reads from belongs to the caller; from_db steals the
// buffer the binding staged, which only the converter can decide to do.
struct BenchNote {
  std::string text;
};

template <>
struct converter<BenchNote> {
  using db_type = std::string;

  static void to_db(BenchNote const& value, std::string& out) {
    out = value.text;
  }

  static BenchNote from_db(std::string&& v) {
    return BenchNote{ std::move(v) };
  }
};

struct BenchConv {
  std::int64_t id = 0;
  std::string name;
  std::int32_t score = 0;
  std::optional<BenchNote> note;
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

orm build_conv_registry(std::string const& conn_string) {
  orm registry(conn_string);
  registry.map<BenchConv>(k_table)
    .primary_key("id", &BenchConv::id)
    .column("name", &BenchConv::name)
    .column("score", &BenchConv::score)
    .column("note", &BenchConv::note);
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

std::vector<BenchConv> make_conv_rows(std::size_t n) {
  std::vector<BenchConv> rows;
  rows.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    BenchConv b;
    b.id = static_cast<std::int64_t>(i);
    b.name = "row-" + std::to_string(i);
    b.score = static_cast<std::int32_t>(i % 1000);
    if (i % 4 != 0) {
      b.note = BenchNote{ "note-" + std::to_string(i) };
    }
    rows.push_back(std::move(b));
  }
  return rows;
}

struct bench_result {
  std::string name;
  std::size_t rows;
  double ms;
};

void report(
  char const* name, std::size_t rows, perf_clock::duration elapsed, int runs,
  std::vector<bench_result>* collect = nullptr) {
  double ms = std::chrono::duration<double, std::milli>(elapsed).count();
  double per_run_rows = static_cast<double>(rows) / runs;
  double krows_per_s = per_run_rows / ms;  // rows/ms == krows/s
  std::printf("%-34s %10.2f ms %12.1f krows/s\n", name, ms, krows_per_s);
  if (collect) {
    collect->push_back({ name, rows, ms });
  }
}

void print_comparison_table(
  std::vector<bench_result> const& orm_results,
  std::vector<bench_result> const& raw_results) {
  std::printf("\n%-28s %10s %12s %10s %12s %6s\n",
    "benchmark", "ORM (ms)", "ORM (k/s)", "Raw (ms)", "Raw (k/s)", "Ratio");
  std::printf("%-28s %10s %12s %10s %12s %6s\n",
    std::string(28, '-').c_str(),
    std::string(10, '-').c_str(),
    std::string(12, '-').c_str(),
    std::string(10, '-').c_str(),
    std::string(12, '-').c_str(),
    std::string(6, '-').c_str());

  for (auto const& orm : orm_results) {
    // Find matching raw result
    auto raw_it = std::find_if(raw_results.begin(), raw_results.end(),
      [&](auto const& r) { return r.name == orm.name; });

    double orm_krows = (orm.rows / orm.ms);
    std::string name = orm.name;
    if (name.length() > 28) {
      name = name.substr(0, 25) + "...";
    }
    if (raw_it != raw_results.end()) {
      double raw_krows = (raw_it->rows / raw_it->ms);
      double ratio = raw_it->ms / orm.ms;  // >1 means ORM is faster
      std::printf("%-28s %8.2f ms %10.1f k/s %8.2f ms %10.1f k/s %5.2fx\n",
        name.c_str(), orm.ms, orm_krows, raw_it->ms, raw_krows, ratio);
    } else {
      std::printf("%-28s %8.2f ms %10.1f k/s %10s %12s %6s\n",
        name.c_str(), orm.ms, orm_krows, "-", "-", "-");
    }
  }

  // Print raw-only results
  for (auto const& raw : raw_results) {
    auto orm_it = std::find_if(orm_results.begin(), orm_results.end(),
      [&](auto const& o) { return o.name == raw.name; });
    if (orm_it == orm_results.end()) {
      double raw_krows = (raw.rows / raw.ms);
      std::string name = raw.name;
      if (name.length() > 28) {
        name = name.substr(0, 25) + "...";
      }
      std::printf("%-28s %10s %12s %8.2f ms %10.1f k/s %6s\n",
        name.c_str(), "-", "-", raw.ms, raw_krows, "-");
    }
  }
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

std::vector<bench_result> run_benchmarks(connection& conn, orm& registry,
  orm& conv_registry, std::string const& conn_string, std::size_t n) {
  std::vector<bench_result> results;
  int const runs = 3;
  std::printf("rows per case: %zu (best of %d runs)\n", n, runs);
  std::printf("\n[uniorm]\n");
  std::printf("%-34s %12s %14s\n", "benchmark", "time", "throughput");

  std::vector<Bench> rows = make_rows(n);

  // Standalone update test: fresh ORM, insert rows, then update only
  {
    orm fresh_registry = build_registry(conn_string);
    conn.execute_update(std::string("DELETE FROM ") + k_table);
    fresh_registry.insert(rows);
    fresh_registry.clear_statement_cache();

    std::vector<Bench> update_rows = rows;
    for (auto& b : update_rows) { b.score += 1; }

    auto start = perf_clock::now();
    fresh_registry.update(update_rows);
    auto elapsed = perf_clock::now() - start;
    double ms = std::chrono::duration<double, std::milli>(elapsed).count();
    std::printf("update (fresh orm, no prior cache)  %8.2f ms %12.1f krows/s\n",
      ms, static_cast<double>(n) / ms);
  }
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
    report("insert (batch)", n, best, 1, &results);
  }
  if (inserted != n) {
    std::printf("FATAL: inserted %zu of %zu rows\n", inserted, n);
    std::exit(1);
  }

  // Batch update benchmark: update score column for all rows
  std::size_t updated = 0;
  {
    registry.clear_statement_cache();
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
    report("update (batch)", n, best, 1, &results);
  }
  if (updated != n) {
    std::printf("FATAL: updated %zu of %zu rows\n", updated, n);
    std::exit(1);
  }

  // Batch delete benchmark: delete half the rows
  std::size_t deleted = 0;
  {
    // Prepare entities for deletion: delete rows with even IDs
    std::vector<Bench> delete_entities;
    for (std::size_t i = 0; i < n; i += 2) {
      Bench b;
      b.id = static_cast<std::int64_t>(i);
      delete_entities.push_back(b);
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
      deleted = registry.remove(delete_entities);
      auto elapsed = perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    report("delete (batch)", delete_entities.size(), best, 1, &results);
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
      [&] { entity_rows = registry.query<Bench>().all().size(); },
      runs),
    runs, &results);
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
  report("query projection (with string)", n,
    best_of(
      [&] { proj_rows = registry.query<bench_row>(select_all).size(); }, runs),
    runs, &results);
  if (proj_rows != n) {
    std::printf("FATAL: projection returned %zu rows\n", proj_rows);
    std::exit(1);
  }

  // Simple POD projection without std::string
  struct bench_simple {
    std::int64_t id;
    std::int32_t score;
  };
  std::size_t simple_rows = 0;
  report("query projection (POD only)", n,
    best_of(
      [&] { simple_rows = registry.query<bench_simple>("SELECT id, score FROM " + std::string(k_table)).size(); }, runs),
    runs, &results);
  if (simple_rows != n) {
    std::printf("FATAL: simple projection returned %zu rows\n", simple_rows);
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
    runs, &results);
  if (dynamic_rows != n) {
    std::printf("FATAL: dynamic query returned %zu rows\n", dynamic_rows);
    std::exit(1);
  }

  report("query one() (direct bind)", 1,
    best_of(
      [&] {
        auto one = registry.query<Bench>().limit(1).one();
        if (!one) {
          std::exit(1);
        }
      },
      runs),
    runs, &results);

  std::int64_t count = 0;
  report("query count()", 1,
    best_of([&] { count = registry.query<Bench>().count(); }, runs),
    runs, &results);
  if (count != static_cast<std::int64_t>(n)) {
    std::printf("FATAL: count() returned %" PRId64 "\n", count);
    std::exit(1);
  }

  std::printf("\n[converter field vs plain field, same columns]\n");

  std::vector<BenchConv> conv_rows = make_conv_rows(n);
  std::size_t conv_inserted = 0;
  {
    perf_clock::duration best = perf_clock::duration::max();
    for (int i = 0; i < runs; ++i) {
      conn.execute_update(std::string("DELETE FROM ") + k_table);
      auto start = perf_clock::now();
      conv_inserted = conv_registry.insert(conv_rows);
      auto elapsed = perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    report("insert (batch, converter field)", n, best, 1, &results);
  }
  if (conv_inserted != n) {
    std::printf("FATAL: converter insert wrote %zu of %zu rows\n",
      conv_inserted, n);
    std::exit(1);
  }

  std::size_t conv_read = 0;
  report("query entity all (converter field)", n,
    best_of(
      [&] {
        conv_read = conv_registry.query<BenchConv>().all().size();
      },
      runs),
    runs, &results);
  if (conv_read != n) {
    std::printf("FATAL: converter entity query returned %zu rows\n", conv_read);
    std::exit(1);
  }
  // Untimed, and a read of its own: a decode that quietly produced nothing
  // would still have counted n rows above.
  for (auto const& b : conv_registry.query<BenchConv>().all()) {
    bool const written = b.id % 4 != 0;
    if (!b.note != !written ||
        (b.note && b.note->text != "note-" + std::to_string(b.id))) {
      std::printf("FATAL: converter decode wrong at id %" PRId64 "\n", b.id);
      std::exit(1);
    }
  }

  struct conv_proj_row {
    std::int64_t id;
    std::string name;
    std::int32_t score;
    std::optional<BenchNote> note;
  };
  std::size_t conv_proj_rows = 0;
  report("query projection (converter field)", n,
    best_of(
      [&] {
        conv_proj_rows = registry.query<conv_proj_row>(select_all).size();
      },
      runs),
    runs, &results);
  if (conv_proj_rows != n) {
    std::printf("FATAL: converter projection returned %zu rows\n",
      conv_proj_rows);
    std::exit(1);
  }

  (void)id_sum;
  return results;
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

std::vector<bench_result> run_raw_benchmarks(std::string const& conn_string, std::size_t n) {
  std::vector<bench_result> results;
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
  report("insert (batch)", n,
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
    1, &results);
  if (batch_inserted != n) {
    std::printf(
      "FATAL: raw batch inserted %zu of %zu rows\n", batch_inserted, n);
    std::exit(1);
  }

  // --- Batch update using SQL_ATTR_PARAMSET_SIZE ---
  std::size_t batch_updated = 0;
  report("update (batch)", n,
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
    runs, &results);
  if (batch_updated != n) {
    std::printf("FATAL: raw batch updated %zu of %zu rows\n", batch_updated, n);
    std::exit(1);
  }

  // --- Batch update with VARCHAR params (to test driver behavior) ---
  std::size_t batch_updated_varchar = 0;
  report("update (paramset+varchar)", n,
    best_of(
      [&] {
        std::vector<SQLINTEGER> scores(batch_size);
        std::vector<char> names(batch_size * 65);
        std::vector<char> notes(batch_size * 65);
        std::vector<SQLBIGINT> ids(batch_size);
        std::vector<SQLLEN> name_inds(batch_size);
        std::vector<SQLLEN> note_inds(batch_size);

        raw_statement upd(rc.dbc,
          std::string("UPDATE ") + k_table + " SET name = ?, score = ?, note = ? WHERE id = ?");

        SQLULEN paramset = batch_size;
        odbc_check(SQLSetStmtAttr(upd.stmt, SQL_ATTR_PARAMSET_SIZE,
                       reinterpret_cast<SQLPOINTER>(paramset), 0),
          SQL_HANDLE_STMT, upd.stmt, "set paramset size");

        odbc_check(SQLBindParameter(upd.stmt, 1, SQL_PARAM_INPUT,
                         SQL_C_CHAR, SQL_VARCHAR, 64, 0, names.data(),
                         65, name_inds.data()),
          SQL_HANDLE_STMT, upd.stmt, "bind paramset name");
        odbc_check(SQLBindParameter(upd.stmt, 2, SQL_PARAM_INPUT,
                         SQL_C_SLONG, SQL_INTEGER, 0, 0, scores.data(),
                         sizeof(SQLINTEGER), nullptr),
          SQL_HANDLE_STMT, upd.stmt, "bind paramset score");
        odbc_check(SQLBindParameter(upd.stmt, 3, SQL_PARAM_INPUT,
                         SQL_C_CHAR, SQL_VARCHAR, 64, 0, notes.data(),
                         65, note_inds.data()),
          SQL_HANDLE_STMT, upd.stmt, "bind paramset note");
        odbc_check(SQLBindParameter(upd.stmt, 4, SQL_PARAM_INPUT,
                         SQL_C_SBIGINT, SQL_BIGINT, 0, 0, ids.data(),
                         sizeof(SQLBIGINT), nullptr),
          SQL_HANDLE_STMT, upd.stmt, "bind paramset id");

        batch_updated_varchar = 0;
        for (std::size_t start = 0; start < n; start += batch_size) {
          std::size_t count = std::min(batch_size, n - start);
          if (count != batch_size) {
            paramset = count;
            odbc_check(SQLSetStmtAttr(upd.stmt, SQL_ATTR_PARAMSET_SIZE,
                           reinterpret_cast<SQLPOINTER>(paramset), 0),
              SQL_HANDLE_STMT, upd.stmt, "set paramset size (tail)");
          }
          for (std::size_t i = 0; i < count; ++i) {
            std::size_t row = start + i;
            ids[i] = static_cast<SQLBIGINT>(row);
            scores[i] = static_cast<SQLINTEGER>(row % 1000 + 1);
            std::snprintf(&names[i * 65], 65, "row-%zu", row);
            name_inds[i] = static_cast<SQLLEN>(std::strlen(&names[i * 65]));
            if (row % 4 == 0) {
              note_inds[i] = SQL_NULL_DATA;
            } else {
              std::snprintf(&notes[i * 65], 65, "note-%zu", row);
              note_inds[i] = static_cast<SQLLEN>(std::strlen(&notes[i * 65]));
            }
          }
          odbc_check(SQLExecute(upd.stmt), SQL_HANDLE_STMT, upd.stmt,
            "execute paramset update varchar");
          batch_updated_varchar += count;
        }
      },
      runs),
    runs, &results);
  if (batch_updated_varchar != n) {
    std::printf("FATAL: raw batch updated (varchar) %zu of %zu rows\n", batch_updated_varchar, n);
    std::exit(1);
  }

  // --- Batch update with VARCHAR, REUSING + REBINDING (like ORM) ---
  std::size_t batch_updated_reuse = 0;
  report("update (paramset+varchar+rebind)", n,
    best_of(
      [&] {
        std::vector<SQLINTEGER> scores(batch_size);
        std::vector<char> names(batch_size * 65);
        std::vector<char> notes(batch_size * 65);
        std::vector<SQLBIGINT> ids(batch_size);
        std::vector<SQLLEN> name_inds(batch_size);
        std::vector<SQLLEN> note_inds(batch_size);

        SQLHSTMT reuse_stmt = SQL_NULL_HANDLE;
        SQLAllocHandle(SQL_HANDLE_STMT, rc.dbc, &reuse_stmt);
        std::string upd_sql = std::string("UPDATE ") + k_table + " SET name = ?, score = ?, note = ? WHERE id = ?";
        odbc_check(SQLPrepare(reuse_stmt,
          reinterpret_cast<SQLCHAR*>(const_cast<char*>(upd_sql.c_str())), SQL_NTS),
          SQL_HANDLE_STMT, reuse_stmt, "prepare reuse update");

        batch_updated_reuse = 0;
        for (std::size_t start = 0; start < n; start += batch_size) {
          std::size_t count = std::min(batch_size, n - start);

          // Reset and rebind (like ORM's finish())
          SQLFreeStmt(reuse_stmt, SQL_RESET_PARAMS);

          SQLULEN paramset = count;
          odbc_check(SQLSetStmtAttr(reuse_stmt, SQL_ATTR_PARAMSET_SIZE,
                         reinterpret_cast<SQLPOINTER>(paramset), 0),
            SQL_HANDLE_STMT, reuse_stmt, "set paramset size");

          odbc_check(SQLBindParameter(reuse_stmt, 1, SQL_PARAM_INPUT,
                           SQL_C_CHAR, SQL_VARCHAR, 64, 0, names.data(),
                           65, name_inds.data()),
            SQL_HANDLE_STMT, reuse_stmt, "bind name");
          odbc_check(SQLBindParameter(reuse_stmt, 2, SQL_PARAM_INPUT,
                           SQL_C_SLONG, SQL_INTEGER, 0, 0, scores.data(),
                           sizeof(SQLINTEGER), nullptr),
            SQL_HANDLE_STMT, reuse_stmt, "bind score");
          odbc_check(SQLBindParameter(reuse_stmt, 3, SQL_PARAM_INPUT,
                           SQL_C_CHAR, SQL_VARCHAR, 64, 0, notes.data(),
                           65, note_inds.data()),
            SQL_HANDLE_STMT, reuse_stmt, "bind note");
          odbc_check(SQLBindParameter(reuse_stmt, 4, SQL_PARAM_INPUT,
                           SQL_C_SBIGINT, SQL_BIGINT, 0, 0, ids.data(),
                           sizeof(SQLBIGINT), nullptr),
            SQL_HANDLE_STMT, reuse_stmt, "bind id");

          for (std::size_t i = 0; i < count; ++i) {
            std::size_t row = start + i;
            ids[i] = static_cast<SQLBIGINT>(row);
            scores[i] = static_cast<SQLINTEGER>(row % 1000 + 1);
            std::snprintf(&names[i * 65], 65, "row-%zu", row);
            name_inds[i] = static_cast<SQLLEN>(std::strlen(&names[i * 65]));
            if (row % 4 == 0) {
              note_inds[i] = SQL_NULL_DATA;
            } else {
              std::snprintf(&notes[i * 65], 65, "note-%zu", row);
              note_inds[i] = static_cast<SQLLEN>(std::strlen(&notes[i * 65]));
            }
          }
          odbc_check(SQLExecute(reuse_stmt), SQL_HANDLE_STMT, reuse_stmt,
            "execute rebind update");
          batch_updated_reuse += count;
        }
        SQLFreeHandle(SQL_HANDLE_STMT, reuse_stmt);
      },
      runs),
    runs, &results);
  if (batch_updated_reuse != n) {
    std::printf("FATAL: raw batch updated (reuse) %zu of %zu rows\n", batch_updated_reuse, n);
    std::exit(1);
  }

  // --- Batch delete using SQL_ATTR_PARAMSET_SIZE ---
  std::size_t batch_deleted = 0;
  std::size_t const delete_count = (n + 1) / 2;  // ceiling division, matches ORM
  report("delete (batch)", delete_count,
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
        for (std::size_t start = 0; start < n; start += 2 * batch_size) {
          std::size_t count = std::min(batch_size, (n - start + 1) / 2);
          if (count == 0) break;
          if (count != batch_size) {
            paramset = count;
            odbc_check(SQLSetStmtAttr(del.stmt, SQL_ATTR_PARAMSET_SIZE,
                           reinterpret_cast<SQLPOINTER>(paramset), 0),
              SQL_HANDLE_STMT, del.stmt, "set paramset size (tail)");
          }
          for (std::size_t i = 0; i < count; ++i) {
            ids[i] = static_cast<SQLBIGINT>(start + 2 * i);  // even IDs only
          }
          odbc_check(SQLExecute(del.stmt), SQL_HANDLE_STMT, del.stmt,
            "execute paramset delete");
          batch_deleted += count;
        }
      },
      runs),
    runs, &results);
  if (batch_deleted != delete_count) {
    std::printf("FATAL: raw batch deleted %zu of %zu rows\n", batch_deleted, delete_count);
    std::exit(1);
  }

  // Re-insert deleted even-ID rows for query benchmarks
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

    for (std::size_t start = 0; start < n; start += 2 * batch_size) {
      std::size_t count = std::min(batch_size, (n - start + 1) / 2);
      if (count == 0) break;
      if (count != batch_size) {
        paramset = count;
        odbc_check(SQLSetStmtAttr(ins.stmt, SQL_ATTR_PARAMSET_SIZE,
                       reinterpret_cast<SQLPOINTER>(paramset), 0),
          SQL_HANDLE_STMT, ins.stmt, "set paramset size (tail)");
      }
      for (std::size_t i = 0; i < count; ++i) {
        std::size_t row = start + 2 * i;  // even IDs only
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
  std::size_t const fetch_size = 1000;
  std::size_t block_rows = 0;
  report("query entity all (fetch only)", n,
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
    runs, &results);
  if (block_rows != n) {
    std::printf("FATAL: raw block fetch returned %zu rows\n", block_rows);
    std::exit(1);
  }

  // Same block fetch, but also assemble Bench entities from the staging
  // buffers — the fair lower bound for what the ORM entity path does.
  std::size_t assembled = 0;
  std::vector<Bench> raw_entities;
  report("query entity all() (direct bind)", n,
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

        q.execute();
        raw_entities.clear();
        raw_entities.reserve(n);
        SQLRETURN fetch_rc = SQLFetch(q.stmt);
        while (fetch_rc == SQL_SUCCESS || fetch_rc == SQL_SUCCESS_WITH_INFO) {
          for (SQLULEN i = 0; i < rows_fetched; ++i) {
            Bench b;
            b.id = ids[i];
            b.name.assign(&names[i * 65],
              static_cast<std::size_t>(name_inds[i]));
            b.score = scores[i];
            if (note_inds[i] != SQL_NULL_DATA) {
              b.note = std::string(&notes[i * 65],
                static_cast<std::size_t>(note_inds[i]));
            }
            raw_entities.push_back(std::move(b));
          }
          fetch_rc = SQLFetch(q.stmt);
        }
        assembled = raw_entities.size();
      },
      runs),
    runs, &results);
  if (assembled != n) {
    std::printf("FATAL: raw assemble returned %zu rows\n", assembled);
    std::exit(1);
  }

  // Single-row fetch, mirroring one() (LIMIT 1 + one SQLFetch).
  report("query one() (direct bind)", 1,
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
    runs, &results);
  return results;
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
    registry.row_array_size(1000);
    orm conv_registry = build_conv_registry(conn_string);
    conv_registry.row_array_size(1000);
    conv_registry.validate();
    auto orm_results =
      run_benchmarks(conn, registry, conv_registry, conn_string, n);
    auto raw_results = run_raw_benchmarks(conn_string, n);
    print_comparison_table(orm_results, raw_results);
    conn.execute_update(std::string("DROP TABLE ") + k_table);
  } catch (std::exception const& e) {
    std::printf("FATAL: unexpected exception: %s\n", e.what());
    return 1;
  }
  return 0;
}
