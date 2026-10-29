// The libpq leg's native half: the same rows moved with libpq called by hand.
//
// Two shapes of write are timed here. One PQexecPrepared per row inside one
// transaction is what the plugin does, since libpq binds no arrays -- paired
// with the uniorm number of the same name, it says what the layer above the
// client costs. One statement carrying a thousand rows' VALUES lists is what a
// client written against this server would do instead, and it stands on its own
// in the table because no path through the plugin reaches it.

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <libpq-fe.h>

#include "perf_common.hpp"

using namespace uniorm;

namespace {

int const k_runs = 3;
std::size_t const k_batch = 1000;

[[noreturn]] void native_fail(std::string const& context, PGresult* result) {
  std::string detail = "no answer";
  if (result != nullptr) {
    detail = PQresultErrorMessage(result);
  }
  perf::fail("raw libpq " + context + ": " + detail);
}

void expect(PGresult* result, ExecStatusType wanted,
  std::string const& what) {
  if (result == nullptr || PQresultStatus(result) != wanted) {
    native_fail(what + " answered " + PQresStatus(result == nullptr
      ? PGRES_FATAL_ERROR : PQresultStatus(result)), result);
  }
}

struct native_session {
  PGconn* made = nullptr;

  explicit native_session(std::string const& connect_string)
    : made(PQconnectdb(connect_string.c_str())) {
    if (PQstatus(made) != CONNECTION_OK) {
      perf::fail("raw libpq connect: " + std::string(PQerrorMessage(made)));
    }
  }

  ~native_session() {
    if (made != nullptr) {
      PQfinish(made);
    }
  }

  void run(std::string const& sql, std::string const& what) {
    PGresult* result = PQexec(made, sql.c_str());
    expect(result, PGRES_COMMAND_OK, what);
    PQclear(result);
  }

  // One scalar out of one row: the count this side reads back to check its own
  // arithmetic against what the server holds.
  std::int64_t scalar(std::string const& sql, std::string const& what) {
    PGresult* result = PQexec(made, sql.c_str());
    expect(result, PGRES_TUPLES_OK, what);
    std::int64_t const value =
      std::strtoll(PQgetvalue(result, 0, 0), nullptr, 10);
    PQclear(result);
    return value;
  }
};

// One statement, prepared under a name of its own so the next prepare does not
// replace it, and deallocated on the way out.
struct native_statement {
  native_session& session;
  std::string name;

  native_statement(native_session& owner, std::string const& label,
    std::string const& sql, int params)
    : session(owner), name("uniorm_perf_raw_" + label) {
    PGresult* made = PQprepare(session.made, name.c_str(), sql.c_str(),
      params, nullptr);
    expect(made, PGRES_COMMAND_OK, "prepare " + label);
    PQclear(made);
  }

  ~native_statement() {
    if (PQstatus(session.made) == CONNECTION_OK) {
      PGresult* dropped =
        PQexec(session.made, ("DEALLOCATE " + name).c_str());
      PQclear(dropped);
    }
  }

  // Text parameters in the order the statement numbers them; a null pointer
  // in the list is the null the server is asked to store.
  PGresult* run(char const* const* values, int count) {
    PGresult* answer = PQexecPrepared(session.made, name.c_str(), count,
      values, nullptr, nullptr, 0);
    if (PQresultStatus(answer) != PGRES_COMMAND_OK &&
        PQresultStatus(answer) != PGRES_TUPLES_OK) {
      native_fail("execute " + name, answer);
    }
    return answer;
  }
};

// The four parameters of one source row. The text lives with the pointers that
// are handed over, so both stay good until the call returns.
struct bound_row {
  std::string id;
  std::string name;
  std::string score;
  std::string note;
  char const* values[4];

  explicit bound_row(Bench const& source)
    : id(std::to_string(source.id)), name(source.name),
      score(std::to_string(source.score)) {
    if (source.note) {
      note = *source.note;
    }
    values[0] = id.c_str();
    values[1] = name.c_str();
    values[2] = score.c_str();
    values[3] = source.note ? note.c_str() : nullptr;
  }
};

// A literal written into a statement this side assembles by hand. Doubling the
// quote is the server's own way of saying one of them belongs to the value.
std::string quoted(std::string const& text) {
  std::string out = "'";
  for (char symbol : text) {
    if (symbol == '\'') {
      out += "''";
    } else {
      out += symbol;
    }
  }
  out += "'";
  return out;
}

std::string row_literal(Bench const& source) {
  return "(" + std::to_string(source.id) + ", " + quoted(source.name) +
    ", " + std::to_string(source.score) + ", " +
    (source.note ? quoted(*source.note) : std::string("NULL")) + ")";
}

}  // namespace

std::vector<perf::bench_result> run_raw_benchmarks(
  perf::leg_face const& face, std::size_t rows) {
  std::vector<perf::bench_result> results;
  std::string const table = perf::table_name();
  std::printf("\n[raw libpq baseline]\n");
  std::printf("%-34s %12s %14s\n", "benchmark", "time", "throughput");

  native_session session(face.connect_string);
  std::vector<Bench> source = perf::make_rows(rows);
  std::vector<Bench> even;
  for (std::size_t index = 0; index < rows; index += 2) {
    even.push_back(source[index]);
  }

  // One prepared statement per row, in one transaction: the calls the plugin's
  // own batch makes, with nothing layered above them.
  std::size_t inserted = 0;
  {
    native_statement ins(session, "insert",
      "INSERT INTO " + table + " (id, name, score, note) VALUES "
      "($1, $2, $3, $4)", 4);
    perf::report("insert (batch)", rows,
      perf::best_of(
        [&] {
          session.run("DELETE FROM " + table, "clear for the raw insert");
        },
        [&] {
          session.run("BEGIN", "open the raw insert transaction");
          inserted = 0;
          for (auto const& entry : source) {
            bound_row bound(entry);
            PQclear(ins.run(bound.values, 4));
            ++inserted;
          }
          session.run("COMMIT", "commit the raw insert transaction");
        },
        k_runs), &results);
  }
  if (inserted != rows) {
    perf::fail("raw insert wrote " + std::to_string(inserted) + " of " +
      std::to_string(rows) + " rows");
  }
  if (session.scalar("SELECT count(*) FROM " + table,
      "count after the raw insert") != static_cast<std::int64_t>(rows)) {
    perf::fail("the table does not hold the rows the raw insert wrote");
  }

  // What a client written against this server would do instead.
  {
    std::size_t listed = 0;
    perf::report("insert (1000-row VALUES list)", rows,
      perf::best_of(
        [&] {
          session.run("DELETE FROM " + table, "clear for the list insert");
        },
        [&] {
          session.run("BEGIN", "open the list insert transaction");
          listed = 0;
          for (std::size_t first = 0; first < rows; first += k_batch) {
            std::size_t const count = std::min(k_batch, rows - first);
            std::string sql =
              "INSERT INTO " + table + " (id, name, score, note) VALUES ";
            for (std::size_t index = 0; index < count; ++index) {
              if (index > 0) {
                sql += ", ";
              }
              sql += row_literal(source[first + index]);
            }
            session.run(sql, "insert a thousand rows at once");
            listed += count;
          }
          session.run("COMMIT", "commit the list insert");
        },
        1), &results);
    if (listed != rows) {
      perf::fail("the list insert wrote " + std::to_string(listed) + " of " +
        std::to_string(rows) + " rows");
    }
  }

  std::size_t updated = 0;
  {
    native_statement upd(session, "update",
      "UPDATE " + table + " SET score = $1 WHERE id = $2", 2);
    perf::report("update (batch)", rows,
      perf::best_of(
        [&] {
          session.run("BEGIN", "open the raw update transaction");
          updated = 0;
          for (auto const& entry : source) {
            std::string score = std::to_string(entry.score + 1);
            std::string id = std::to_string(entry.id);
            char const* values[2] = { score.c_str(), id.c_str() };
            PQclear(upd.run(values, 2));
            ++updated;
          }
          session.run("COMMIT", "commit the raw update transaction");
        },
        k_runs), &results);
  }
  if (updated != rows) {
    perf::fail("raw update reached " + std::to_string(updated) + " of " +
      std::to_string(rows) + " rows");
  }

  std::size_t const delete_count = even.size();
  std::size_t deleted = 0;
  {
    native_statement del(session, "delete",
      "DELETE FROM " + table + " WHERE id = $1", 1);
    native_statement restore(session, "restore",
      "INSERT INTO " + table + " (id, name, score, note) VALUES "
      "($1, $2, $3, $4)", 4);
    // The rows a pass removes go back between passes, untimed, so each pass is
    // asked to delete the same number of them rather than nothing at all.
    auto put_back = [&] {
      for (auto const& entry : even) {
        bound_row bound(entry);
        PQclear(restore.run(bound.values, 4));
      }
    };
    perf::perf_clock::duration best = perf::perf_clock::duration::max();
    for (int pass = 0; pass < k_runs; ++pass) {
      if (pass > 0) {
        put_back();
      }
      auto start = perf::perf_clock::now();
      session.run("BEGIN", "open the raw delete transaction");
      deleted = 0;
      for (auto const& entry : even) {
        std::string id = std::to_string(entry.id);
        char const* values[1] = { id.c_str() };
        PQclear(del.run(values, 1));
        ++deleted;
      }
      session.run("COMMIT", "commit the raw delete transaction");
      auto const elapsed = perf::perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    perf::report("delete (batch)", delete_count, best, &results);
    put_back();
  }
  if (deleted != delete_count) {
    perf::fail("raw delete removed " + std::to_string(deleted) + " of " +
      std::to_string(delete_count) + " rows");
  }

  std::string const select_all =
    "SELECT id, name, score, note FROM " + table;

  // The whole answer arrives at once, so the two read cases differ only in
  // what this side does with it: touch every field, or build a row out of one.
  std::size_t fetched_rows = 0;
  {
    perf::report("query entity all (fetch only)", rows,
      perf::best_of(
        [&] {
          PGresult* answer = PQexec(session.made, select_all.c_str());
          expect(answer, PGRES_TUPLES_OK, "read the table");
          fetched_rows = static_cast<std::size_t>(PQntuples(answer));
          std::size_t touched = 0;
          for (int position = 0; position < PQntuples(answer); ++position) {
            for (int column = 0; column < PQnfields(answer); ++column) {
              if (!PQgetisnull(answer, position, column)) {
                touched +=
                  std::strlen(PQgetvalue(answer, position, column));
              }
            }
          }
          if (touched == 0) {
            perf::fail("raw fetch read no field at all");
          }
          PQclear(answer);
        },
        k_runs), &results);
  }
  if (fetched_rows != rows) {
    perf::fail("raw fetch returned " + std::to_string(fetched_rows) + " rows");
  }

  std::size_t assembled = 0;
  {
    std::vector<Bench> entities;
    perf::report("query entity all() (direct bind)", rows,
      perf::best_of(
        [&] {
          PGresult* answer = PQexec(session.made, select_all.c_str());
          expect(answer, PGRES_TUPLES_OK, "read the table for entities");
          entities.clear();
          entities.reserve(static_cast<std::size_t>(PQntuples(answer)));
          for (int position = 0; position < PQntuples(answer); ++position) {
            Bench entry;
            entry.id =
              std::strtoll(PQgetvalue(answer, position, 0), nullptr, 10);
            entry.name = PQgetvalue(answer, position, 1);
            entry.score = static_cast<std::int32_t>(
              std::strtol(PQgetvalue(answer, position, 2), nullptr, 10));
            if (!PQgetisnull(answer, position, 3)) {
              entry.note = std::string(PQgetvalue(answer, position, 3));
            }
            entities.push_back(std::move(entry));
          }
          PQclear(answer);
          assembled = entities.size();
        },
        k_runs), &results);
  }
  if (assembled != rows) {
    perf::fail("raw assemble returned " + std::to_string(assembled) + " rows");
  }

  {
    native_statement one(session, "one", select_all + " LIMIT $1", 1);
    char const* limit = "1";
    perf::report("query one() (direct bind)", 1,
      perf::best_of(
        [&] {
          PGresult* answer = one.run(&limit, 1);
          if (PQntuples(answer) != 1) {
            perf::fail("raw one() answered with no row");
          }
          PQclear(answer);
        },
        k_runs), &results);
  }

  {
    native_statement total(session, "count", "SELECT count(*) FROM " + table,
      0);
    perf::report("query count()", 1,
      perf::best_of(
        [&] {
          PGresult* answer = total.run(nullptr, 0);
          if (std::strtoll(PQgetvalue(answer, 0, 0), nullptr, 10) !=
              static_cast<std::int64_t>(rows)) {
            perf::fail("raw count() answered a different table");
          }
          PQclear(answer);
        },
        k_runs), &results);
  }
  return results;
}
