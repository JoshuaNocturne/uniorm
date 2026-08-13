// Performance benchmarks against a live database via ODBC DSN.
// DSN comes from UNIORM_IT_DSN (default: docker_maria); row count from
// UNIORM_PERF_ROWS (default: 10000). Returns 77 (ctest SKIP) when the
// database is unreachable. Compares the query materialization paths:
// entity direct binding, aggregate projection, and dynamic rows, plus a
// raw ODBC baseline that mirrors uniorm's ODBC call patterns (multi-row
// VALUES batch insert, SQLBindCol scans) for overhead comparison.

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

orm build_registry() {
  orm registry;
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
  report("insert (batch)", n,
    best_of([&] { inserted = conn.insert(registry, rows); }, 1), 1);
  if (inserted != n) {
    std::printf("FATAL: inserted %zu of %zu rows\n", inserted, n);
    std::exit(1);
  }

  std::string select_all =
    std::string("SELECT id, name, score, note FROM ") + k_table;

  std::size_t entity_rows = 0;
  report("query entity all() (direct bind)", n,
    best_of(
      [&] { entity_rows = conn.query(registry).of<Bench>().all().size(); },
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
        auto one = conn.query(registry).of<Bench>().limit(1).one();
        if (!one) {
          std::exit(1);
        }
      },
      runs),
    runs);

  std::int64_t count = 0;
  report("query count()", 1,
    best_of([&] { count = conn.query(registry).of<Bench>().count(); }, runs),
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

  // Multi-row VALUES insert mirroring uniorm insert_batch call-for-call:
  // same SQL shape, same 4096-placeholder chunking, one SQLBindParameter
  // per value, one SQLExecute per batch, single transaction.
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
        std::size_t const cols = 4;
        std::size_t const placeholders_cap = 4096;
        std::size_t const rows_per_batch = placeholders_cap / cols;
        std::vector<SQLBIGINT> ids(rows_per_batch);
        std::vector<char> names(rows_per_batch * 65);
        std::vector<SQLINTEGER> scores(rows_per_batch);
        std::vector<char> notes(rows_per_batch * 65);
        std::vector<SQLLEN> name_inds(rows_per_batch);
        std::vector<SQLLEN> note_inds(rows_per_batch);
        batch_inserted = 0;
        for (std::size_t start = 0; start < n; start += rows_per_batch) {
          std::size_t batch = std::min(rows_per_batch, n - start);
          std::string sql = std::string("INSERT INTO ") + k_table +
                            " (id, name, score, note) VALUES ";
          for (std::size_t i = 0; i < batch; ++i) {
            if (i != 0) {
              sql += ", ";
            }
            sql += "(?, ?, ?, ?)";
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
          raw_statement ins(rc.dbc, sql);
          for (std::size_t i = 0; i < batch; ++i) {
            SQLUSMALLINT base = static_cast<SQLUSMALLINT>(i * cols + 1);
            odbc_check(SQLBindParameter(ins.stmt, base, SQL_PARAM_INPUT,
                         SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &ids[i], 0, nullptr),
              SQL_HANDLE_STMT, ins.stmt, "bind batch id");
            odbc_check(
              SQLBindParameter(ins.stmt, static_cast<SQLUSMALLINT>(base + 1),
                SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0, &names[i * 65],
                65, &name_inds[i]),
              SQL_HANDLE_STMT, ins.stmt, "bind batch name");
            odbc_check(
              SQLBindParameter(ins.stmt, static_cast<SQLUSMALLINT>(base + 2),
                SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &scores[i], 0,
                nullptr),
              SQL_HANDLE_STMT, ins.stmt, "bind batch score");
            odbc_check(
              SQLBindParameter(ins.stmt, static_cast<SQLUSMALLINT>(base + 3),
                SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0, &notes[i * 65],
                65, &note_inds[i]),
              SQL_HANDLE_STMT, ins.stmt, "bind batch note");
          }
          odbc_check(SQLExecute(ins.stmt), SQL_HANDLE_STMT, ins.stmt,
            "execute batch insert");
          batch_inserted += batch;
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

  std::string select_all =
    std::string("SELECT id, name, score, note FROM ") + k_table;

  // Full scan with SQLBindCol onto plain buffers.
  std::size_t bound_rows = 0;
  report("query (SQLBindCol)", n,
    best_of(
      [&] {
        raw_statement q(rc.dbc, select_all);
        SQLBIGINT id = 0;
        char name[65];
        SQLINTEGER score = 0;
        char note[65];
        SQLLEN id_ind = 0, name_ind = 0, score_ind = 0, note_ind = 0;
        odbc_check(
          SQLBindCol(q.stmt, 1, SQL_C_SBIGINT, &id, sizeof(id), &id_ind),
          SQL_HANDLE_STMT, q.stmt, "bindcol 1");
        odbc_check(
          SQLBindCol(q.stmt, 2, SQL_C_CHAR, name, sizeof(name), &name_ind),
          SQL_HANDLE_STMT, q.stmt, "bindcol 2");
        odbc_check(
          SQLBindCol(q.stmt, 3, SQL_C_SLONG, &score, sizeof(score), &score_ind),
          SQL_HANDLE_STMT, q.stmt, "bindcol 3");
        odbc_check(
          SQLBindCol(q.stmt, 4, SQL_C_CHAR, note, sizeof(note), &note_ind),
          SQL_HANDLE_STMT, q.stmt, "bindcol 4");
        bound_rows = 0;
        q.execute();
        while (SQLFetch(q.stmt) == SQL_SUCCESS) {
          ++bound_rows;
        }
      },
      runs),
    runs);
  if (bound_rows != n) {
    std::printf("FATAL: raw bindcol query returned %zu rows\n", bound_rows);
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
  std::string dsn = dsn_env && *dsn_env ? dsn_env : "docker_maria";
  std::string user = user_env && *user_env ? user_env : "Joshua";
  std::string pwd = pwd_env && *pwd_env ? pwd_env : "joshua";
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
    orm registry = build_registry();
    run_benchmarks(conn, registry, n);
    run_raw_benchmarks(conn_string, n);
    conn.execute_update(std::string("DROP TABLE ") + k_table);
  } catch (std::exception const& e) {
    std::printf("FATAL: unexpected exception: %s\n", e.what());
    return 1;
  }
  return 0;
}
