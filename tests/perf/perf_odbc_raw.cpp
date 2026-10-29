// The ODBC leg's native half: the same bytes moved with driver calls made by
// hand, so the uniorm numbers beside them say what the layer above a driver
// costs on the server this leg is sitting on. Array binding and block fetching
// are used here because a driver offers both and this backend does not ask.

#include <cstddef>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

#include "perf_common.hpp"

using namespace uniorm;

namespace {

[[noreturn]] void odbc_fail(
  SQLSMALLINT handle_type, SQLHANDLE handle, char const* context) {
  SQLCHAR state[16] = {};
  SQLCHAR text[512] = {};
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  SQLGetDiagRec(
    handle_type, handle, 1, state, &native, text, sizeof(text), &len);
  perf::fail(std::string("raw ODBC ") + context + ": [" + std::string(
    reinterpret_cast<char const*>(state)) + "] " + std::string(
    reinterpret_cast<char const*>(text)));
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

  explicit raw_connection(std::string const& connect_string) {
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
    SQLCHAR out[1024];
    SQLSMALLINT out_len = 0;
    odbc_check(
      SQLDriverConnect(dbc, nullptr,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(connect_string.c_str())),
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

// The four arrays one batch of rows is staged into: the two numbers in their
// own elements, the two texts in fixed 65-byte slots with the indicator
// saying whether a slot holds a value at all.
struct batch_buffers {
  std::vector<SQLBIGINT> ids;
  std::vector<SQLINTEGER> scores;
  std::vector<char> names;
  std::vector<char> notes;
  std::vector<SQLLEN> name_inds;
  std::vector<SQLLEN> note_inds;

  explicit batch_buffers(std::size_t count)
    : ids(count), scores(count), names(count * 65), notes(count * 65),
      name_inds(count), note_inds(count) {}

  void stage(std::size_t first, std::size_t count, int score_shift) {
    for (std::size_t index = 0; index < count; ++index) {
      std::size_t const source = first + index;
      ids[index] = static_cast<SQLBIGINT>(source);
      scores[index] =
        static_cast<SQLINTEGER>(source % 1000 + score_shift);
      std::snprintf(&names[index * 65], 65, "row-%zu", source);
      name_inds[index] =
        static_cast<SQLLEN>(std::strlen(&names[index * 65]));
      if (source % 4 == 0) {
        note_inds[index] = SQL_NULL_DATA;
      } else {
        std::snprintf(&notes[index * 65], 65, "note-%zu", source);
        note_inds[index] = static_cast<SQLLEN>(std::strlen(
          &notes[index * 65]));
      }
    }
  }
};

void bind_batch(raw_statement& ins, batch_buffers& buffers) {
  odbc_check(SQLBindParameter(ins.stmt, 1, SQL_PARAM_INPUT,
                   SQL_C_SBIGINT, SQL_BIGINT, 0, 0, buffers.ids.data(),
                   sizeof(SQLBIGINT), nullptr),
    SQL_HANDLE_STMT, ins.stmt, "bind paramset id");
  odbc_check(SQLBindParameter(ins.stmt, 2, SQL_PARAM_INPUT,
                   SQL_C_CHAR, SQL_VARCHAR, 64, 0, buffers.names.data(),
                   65, buffers.name_inds.data()),
    SQL_HANDLE_STMT, ins.stmt, "bind paramset name");
  odbc_check(SQLBindParameter(ins.stmt, 3, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, buffers.scores.data(),
                   sizeof(SQLINTEGER), nullptr),
    SQL_HANDLE_STMT, ins.stmt, "bind paramset score");
  odbc_check(SQLBindParameter(ins.stmt, 4, SQL_PARAM_INPUT,
                   SQL_C_CHAR, SQL_VARCHAR, 64, 0, buffers.notes.data(),
                   65, buffers.note_inds.data()),
    SQL_HANDLE_STMT, ins.stmt, "bind paramset note");
}

void set_paramset(raw_statement& stmt, std::size_t count, char const* context) {
  odbc_check(SQLSetStmtAttr(stmt.stmt, SQL_ATTR_PARAMSET_SIZE,
                   reinterpret_cast<SQLPOINTER>(count), 0),
    SQL_HANDLE_STMT, stmt.stmt, context);
}

  // The even ids, in the driver's own array binding.
void write_even_rows(raw_connection& session, std::string const& table,
  std::size_t rows, std::size_t batch_size) {
  raw_statement ins(session.dbc,
    "INSERT INTO " + table + " (id, name, score, note) VALUES "
    "(?, ?, ?, ?)");
  batch_buffers buffers(batch_size);
  set_paramset(ins, batch_size, "set paramset size");
  bind_batch(ins, buffers);

  for (std::size_t first = 0; first < rows; first += 2 * batch_size) {
    std::size_t const count = std::min(batch_size, (rows - first + 1) / 2);
    if (count == 0) {
      break;
    }
    if (count != batch_size) {
      set_paramset(ins, count, "set paramset size (tail)");
    }
    for (std::size_t index = 0; index < count; ++index) {
      std::size_t const source = first + 2 * index;  // even ids only
      buffers.ids[index] = static_cast<SQLBIGINT>(source);
      std::snprintf(&buffers.names[index * 65], 65, "row-%zu", source);
      buffers.name_inds[index] = SQL_NTS;
      buffers.scores[index] = static_cast<SQLINTEGER>(source % 1000);
      if (source % 4 == 0) {
        buffers.note_inds[index] = SQL_NULL_DATA;
      } else {
        std::snprintf(&buffers.notes[index * 65], 65, "note-%zu", source);
        buffers.note_inds[index] = SQL_NTS;
      }
    }
    odbc_check(SQLExecute(ins.stmt), SQL_HANDLE_STMT, ins.stmt,
      "re-insert the even rows");
  }
}

}  // namespace

std::vector<perf::bench_result> run_raw_benchmarks(
  perf::leg_face const& face, std::size_t rows) {
  std::vector<perf::bench_result> results;
  int const runs = 3;
  std::string const table = perf::table_name();
  std::printf("\n[raw ODBC baseline]\n");
  std::printf("%-34s %12s %14s\n", "benchmark", "time", "throughput");

  raw_connection session(face.connect_string);
  std::size_t const batch_size = 1000;

  {
    raw_statement clear(session.dbc, "DELETE FROM " + table);
    odbc_check(SQLExecute(clear.stmt), SQL_HANDLE_STMT, clear.stmt,
      "delete before raw batch insert");
  }

  std::size_t batch_inserted = 0;
  {
    raw_statement ins(session.dbc,
      "INSERT INTO " + table + " (id, name, score, note) VALUES "
      "(?, ?, ?, ?)");
    batch_buffers buffers(batch_size);
    perf::report("insert (batch)", rows,
      perf::best_of(
        [&] {
          SQLSetConnectAttr(session.dbc, SQL_ATTR_AUTOCOMMIT,
            (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
          set_paramset(ins, batch_size, "set paramset size");
          bind_batch(ins, buffers);

          batch_inserted = 0;
          for (std::size_t first = 0; first < rows; first += batch_size) {
            std::size_t const count = std::min(batch_size, rows - first);
            if (count != batch_size) {
              set_paramset(ins, count, "set paramset size (tail)");
            }
            buffers.stage(first, count, 0);
            odbc_check(SQLExecute(ins.stmt), SQL_HANDLE_STMT, ins.stmt,
              "execute paramset insert");
            batch_inserted += count;
          }
          odbc_check(SQLEndTran(SQL_HANDLE_DBC, session.dbc, SQL_COMMIT),
            SQL_HANDLE_DBC, session.dbc, "commit raw batch insert");
          SQLSetConnectAttr(session.dbc, SQL_ATTR_AUTOCOMMIT,
            (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);
        },
        1), &results);
  }
  if (batch_inserted != rows) {
    perf::fail("raw batch inserted " + std::to_string(batch_inserted) +
      " of " + std::to_string(rows) + " rows");
  }

  std::size_t batch_updated = 0;
  {
    std::vector<SQLINTEGER> scores(batch_size);
    std::vector<SQLBIGINT> ids(batch_size);

    raw_statement upd(session.dbc,
      "UPDATE " + table + " SET score = ? WHERE id = ?");
    perf::report("update (batch)", rows,
      perf::best_of(
        [&] {
          set_paramset(upd, batch_size, "set paramset size");
          odbc_check(SQLBindParameter(upd.stmt, 1, SQL_PARAM_INPUT,
                           SQL_C_SLONG, SQL_INTEGER, 0, 0, scores.data(),
                           sizeof(SQLINTEGER), nullptr),
            SQL_HANDLE_STMT, upd.stmt, "bind paramset score");
          odbc_check(SQLBindParameter(upd.stmt, 2, SQL_PARAM_INPUT,
                           SQL_C_SBIGINT, SQL_BIGINT, 0, 0, ids.data(),
                           sizeof(SQLBIGINT), nullptr),
            SQL_HANDLE_STMT, upd.stmt, "bind paramset id");

          batch_updated = 0;
          for (std::size_t first = 0; first < rows; first += batch_size) {
            std::size_t const count = std::min(batch_size, rows - first);
            if (count != batch_size) {
              set_paramset(upd, count, "set paramset size (tail)");
            }
            for (std::size_t index = 0; index < count; ++index) {
              ids[index] = static_cast<SQLBIGINT>(first + index);
              scores[index] =
                static_cast<SQLINTEGER>((first + index) % 1000 + 1);
            }
            odbc_check(SQLExecute(upd.stmt), SQL_HANDLE_STMT, upd.stmt,
              "execute paramset update");
            batch_updated += count;
          }
        },
        runs), &results);
  }
  if (batch_updated != rows) {
    perf::fail("raw batch updated " + std::to_string(batch_updated) +
      " of " + std::to_string(rows) + " rows");
  }

  // The same update with all three written columns, which is what a driver
  // costs when the text of a row travels with its numbers.
  std::size_t batch_updated_varchar = 0;
  {
    batch_buffers buffers(batch_size);
    raw_statement upd(session.dbc,
      "UPDATE " + table + " SET name = ?, score = ?, note = ? WHERE id = ?");
    perf::report("update (paramset+varchar)", rows,
      perf::best_of(
        [&] {
          set_paramset(upd, batch_size, "set paramset size");
          odbc_check(SQLBindParameter(upd.stmt, 1, SQL_PARAM_INPUT,
                           SQL_C_CHAR, SQL_VARCHAR, 64, 0,
                           buffers.names.data(), 65,
                           buffers.name_inds.data()),
            SQL_HANDLE_STMT, upd.stmt, "bind paramset name");
          odbc_check(SQLBindParameter(upd.stmt, 2, SQL_PARAM_INPUT,
                           SQL_C_SLONG, SQL_INTEGER, 0, 0,
                           buffers.scores.data(), sizeof(SQLINTEGER), nullptr),
            SQL_HANDLE_STMT, upd.stmt, "bind paramset score");
          odbc_check(SQLBindParameter(upd.stmt, 3, SQL_PARAM_INPUT,
                           SQL_C_CHAR, SQL_VARCHAR, 64, 0,
                           buffers.notes.data(), 65,
                           buffers.note_inds.data()),
            SQL_HANDLE_STMT, upd.stmt, "bind paramset note");
          odbc_check(SQLBindParameter(upd.stmt, 4, SQL_PARAM_INPUT,
                           SQL_C_SBIGINT, SQL_BIGINT, 0, 0,
                           buffers.ids.data(), sizeof(SQLBIGINT), nullptr),
            SQL_HANDLE_STMT, upd.stmt, "bind paramset id");

          batch_updated_varchar = 0;
          for (std::size_t first = 0; first < rows; first += batch_size) {
            std::size_t const count = std::min(batch_size, rows - first);
            if (count != batch_size) {
              set_paramset(upd, count, "set paramset size (tail)");
            }
            buffers.stage(first, count, 1);
            odbc_check(SQLExecute(upd.stmt), SQL_HANDLE_STMT, upd.stmt,
              "execute paramset update varchar");
            batch_updated_varchar += count;
          }
        },
        runs), &results);
  }
  if (batch_updated_varchar != rows) {
    perf::fail("raw batch updated (varchar) " +
      std::to_string(batch_updated_varchar) + " of " +
      std::to_string(rows) + " rows");
  }

  // The same bytes again, this time with one handle reset and rebound between
  // batches -- the shape a cached statement of the library's own takes.
  std::size_t batch_updated_reuse = 0;
  {
    std::string const upd_sql =
      "UPDATE " + table + " SET name = ?, score = ?, note = ? WHERE id = ?";
    raw_statement reuse(session.dbc, upd_sql);
    batch_buffers buffers(batch_size);
    perf::report("update (paramset+varchar+rebind)", rows,
      perf::best_of(
        [&] {
          batch_updated_reuse = 0;
          for (std::size_t first = 0; first < rows; first += batch_size) {
            std::size_t const count = std::min(batch_size, rows - first);
            SQLFreeStmt(reuse.stmt, SQL_RESET_PARAMS);
            set_paramset(reuse, count, "set paramset size");
            odbc_check(SQLBindParameter(reuse.stmt, 1, SQL_PARAM_INPUT,
                             SQL_C_CHAR, SQL_VARCHAR, 64, 0,
                             buffers.names.data(), 65,
                             buffers.name_inds.data()),
              SQL_HANDLE_STMT, reuse.stmt, "bind name");
            odbc_check(SQLBindParameter(reuse.stmt, 2, SQL_PARAM_INPUT,
                             SQL_C_SLONG, SQL_INTEGER, 0, 0,
                             buffers.scores.data(), sizeof(SQLINTEGER),
                             nullptr),
              SQL_HANDLE_STMT, reuse.stmt, "bind score");
            odbc_check(SQLBindParameter(reuse.stmt, 3, SQL_PARAM_INPUT,
                             SQL_C_CHAR, SQL_VARCHAR, 64, 0,
                             buffers.notes.data(), 65,
                             buffers.note_inds.data()),
              SQL_HANDLE_STMT, reuse.stmt, "bind note");
            odbc_check(SQLBindParameter(reuse.stmt, 4, SQL_PARAM_INPUT,
                             SQL_C_SBIGINT, SQL_BIGINT, 0, 0,
                             buffers.ids.data(), sizeof(SQLBIGINT), nullptr),
              SQL_HANDLE_STMT, reuse.stmt, "bind id");

            buffers.stage(first, count, 1);
            odbc_check(SQLExecute(reuse.stmt), SQL_HANDLE_STMT, reuse.stmt,
              "execute rebind update");
            batch_updated_reuse += count;
          }
        },
        runs), &results);
  }
  if (batch_updated_reuse != rows) {
    perf::fail("raw batch updated (reuse) " +
      std::to_string(batch_updated_reuse) + " of " +
      std::to_string(rows) + " rows");
  }

  std::size_t const delete_count = (rows + 1) / 2;  // ceiling, as the ORM side
  std::vector<SQLBIGINT> delete_ids(batch_size);
  std::size_t batch_deleted = 0;
  {
    raw_statement del(session.dbc, "DELETE FROM " + table + " WHERE id = ?");
    // A pass takes the even rows out, so they go back between passes, untimed:
    // timing the same request against an emptied table would report a speed
    // that pass never had.
    perf::perf_clock::duration best = perf::perf_clock::duration::max();
    for (int pass = 0; pass < runs; ++pass) {
      if (pass > 0) {
        write_even_rows(session, table, rows, batch_size);
      }
      auto const start = perf::perf_clock::now();
      batch_deleted = 0;
      set_paramset(del, batch_size, "set paramset size");
      odbc_check(SQLBindParameter(del.stmt, 1, SQL_PARAM_INPUT,
                       SQL_C_SBIGINT, SQL_BIGINT, 0, 0,
                       delete_ids.data(), sizeof(SQLBIGINT), nullptr),
        SQL_HANDLE_STMT, del.stmt, "bind paramset id");

      for (std::size_t first = 0; first < rows; first += 2 * batch_size) {
        std::size_t const count =
          std::min(batch_size, (rows - first + 1) / 2);
        if (count == 0) {
          break;
        }
        if (count != batch_size) {
          set_paramset(del, count, "set paramset size (tail)");
        }
        for (std::size_t index = 0; index < count; ++index) {
          delete_ids[index] =
            static_cast<SQLBIGINT>(first + 2 * index);  // even ids only
        }
        odbc_check(SQLExecute(del.stmt), SQL_HANDLE_STMT, del.stmt,
          "execute paramset delete");
        batch_deleted += count;
      }
      auto const elapsed = perf::perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    perf::report("delete (batch)", delete_count, best, &results);
  }
  if (batch_deleted != delete_count) {
    perf::fail("raw batch deleted " + std::to_string(batch_deleted) + " of " +
      std::to_string(delete_count) + " rows");
  }

  // The even rows go back, so the read cases below see the same table the
  // uniorm half read.
  write_even_rows(session, table, rows, batch_size);

  std::string const select_all =
    "SELECT id, name, score, note FROM " + table;
  std::size_t const fetch_size = 1000;

  // Block fetch, nothing done with the rows: what the transport alone costs.
  std::size_t block_rows = 0;
  {
    raw_statement query(session.dbc, select_all);
    std::vector<SQLBIGINT> ids(fetch_size);
    std::vector<char> names(fetch_size * 65);
    std::vector<SQLINTEGER> scores(fetch_size);
    std::vector<char> notes(fetch_size * 65);
    std::vector<SQLLEN> id_inds(fetch_size);
    std::vector<SQLLEN> name_inds(fetch_size);
    std::vector<SQLLEN> score_inds(fetch_size);
    std::vector<SQLLEN> note_inds(fetch_size);
    SQLULEN rows_fetched = 0;

    odbc_check(SQLSetStmtAttr(query.stmt, SQL_ATTR_ROW_ARRAY_SIZE,
                   reinterpret_cast<SQLPOINTER>(fetch_size), 0),
      SQL_HANDLE_STMT, query.stmt, "set row array size");
    odbc_check(SQLSetStmtAttr(query.stmt, SQL_ATTR_ROWS_FETCHED_PTR,
                   &rows_fetched, 0),
      SQL_HANDLE_STMT, query.stmt, "set rows fetched ptr");
    odbc_check(SQLBindCol(query.stmt, 1, SQL_C_SBIGINT, ids.data(),
                   sizeof(SQLBIGINT), id_inds.data()),
      SQL_HANDLE_STMT, query.stmt, "bindcol 1");
    odbc_check(SQLBindCol(query.stmt, 2, SQL_C_CHAR, names.data(),
                   65, name_inds.data()),
      SQL_HANDLE_STMT, query.stmt, "bindcol 2");
    odbc_check(SQLBindCol(query.stmt, 3, SQL_C_SLONG, scores.data(),
                   sizeof(SQLINTEGER), score_inds.data()),
      SQL_HANDLE_STMT, query.stmt, "bindcol 3");
    odbc_check(SQLBindCol(query.stmt, 4, SQL_C_CHAR, notes.data(),
                   65, note_inds.data()),
      SQL_HANDLE_STMT, query.stmt, "bindcol 4");

    perf::report("query entity all (fetch only)", rows,
      perf::best_of(
        [&] {
          query.execute();
          block_rows = 0;
          SQLRETURN status = SQLFetch(query.stmt);
          while (status == SQL_SUCCESS || status == SQL_SUCCESS_WITH_INFO) {
            block_rows += rows_fetched;
            status = SQLFetch(query.stmt);
          }
        },
        runs), &results);
  }
  if (block_rows != rows) {
    perf::fail("raw block fetch returned " + std::to_string(block_rows) +
      " rows");
  }

  // The same blocks, with the entities assembled out of them -- the fair
  // lower bound for what the library's entity path is asked to produce.
  std::size_t assembled = 0;
  {
    raw_statement query(session.dbc, select_all);
    std::vector<SQLBIGINT> ids(fetch_size);
    std::vector<char> names(fetch_size * 65);
    std::vector<SQLINTEGER> scores(fetch_size);
    std::vector<char> notes(fetch_size * 65);
    std::vector<SQLLEN> id_inds(fetch_size);
    std::vector<SQLLEN> name_inds(fetch_size);
    std::vector<SQLLEN> score_inds(fetch_size);
    std::vector<SQLLEN> note_inds(fetch_size);
    SQLULEN rows_fetched = 0;
    std::vector<Bench> entities;

    odbc_check(SQLSetStmtAttr(query.stmt, SQL_ATTR_ROW_ARRAY_SIZE,
                   reinterpret_cast<SQLPOINTER>(fetch_size), 0),
      SQL_HANDLE_STMT, query.stmt, "set row array size");
    odbc_check(SQLSetStmtAttr(query.stmt, SQL_ATTR_ROWS_FETCHED_PTR,
                   &rows_fetched, 0),
      SQL_HANDLE_STMT, query.stmt, "set rows fetched ptr");
    odbc_check(SQLBindCol(query.stmt, 1, SQL_C_SBIGINT, ids.data(),
                   sizeof(SQLBIGINT), id_inds.data()),
      SQL_HANDLE_STMT, query.stmt, "bindcol 1");
    odbc_check(SQLBindCol(query.stmt, 2, SQL_C_CHAR, names.data(),
                   65, name_inds.data()),
      SQL_HANDLE_STMT, query.stmt, "bindcol 2");
    odbc_check(SQLBindCol(query.stmt, 3, SQL_C_SLONG, scores.data(),
                   sizeof(SQLINTEGER), score_inds.data()),
      SQL_HANDLE_STMT, query.stmt, "bindcol 3");
    odbc_check(SQLBindCol(query.stmt, 4, SQL_C_CHAR, notes.data(),
                   65, note_inds.data()),
      SQL_HANDLE_STMT, query.stmt, "bindcol 4");

    perf::report("query entity all() (direct bind)", rows,
      perf::best_of(
        [&] {
          query.execute();
          entities.clear();
          entities.reserve(rows);
          SQLRETURN status = SQLFetch(query.stmt);
          while (status == SQL_SUCCESS || status == SQL_SUCCESS_WITH_INFO) {
            for (SQLULEN slot = 0; slot < rows_fetched; ++slot) {
              Bench row;
              row.id = ids[slot];
              row.name.assign(&names[slot * 65],
                static_cast<std::size_t>(name_inds[slot]));
              row.score = scores[slot];
              if (note_inds[slot] != SQL_NULL_DATA) {
                row.note = std::string(&notes[slot * 65],
                  static_cast<std::size_t>(note_inds[slot]));
              }
              entities.push_back(std::move(row));
            }
            status = SQLFetch(query.stmt);
          }
          assembled = entities.size();
        },
        runs), &results);
  }
  if (assembled != rows) {
    perf::fail("raw assemble returned " + std::to_string(assembled) + " rows");
  }

  // One row, the way one() asks for it.
  {
    raw_statement query(session.dbc, select_all + " LIMIT 1");
    SQLBIGINT id = 0;
    char name[65];
    SQLINTEGER score = 0;
    char note[65];
    SQLLEN id_ind = 0;
    SQLLEN name_ind = 0;
    SQLLEN score_ind = 0;
    SQLLEN note_ind = 0;
    SQLBindCol(query.stmt, 1, SQL_C_SBIGINT, &id, sizeof(id), &id_ind);
    SQLBindCol(query.stmt, 2, SQL_C_CHAR, name, sizeof(name), &name_ind);
    SQLBindCol(query.stmt, 3, SQL_C_SLONG, &score, sizeof(score), &score_ind);
    SQLBindCol(query.stmt, 4, SQL_C_CHAR, note, sizeof(note), &note_ind);

    perf::report("query one() (direct bind)", 1,
      perf::best_of(
        [&] {
          query.execute();
          if (SQLFetch(query.stmt) != SQL_SUCCESS) {
            perf::fail("raw one() answered with no row");
          }
        },
        runs), &results);
  }
  return results;
}
