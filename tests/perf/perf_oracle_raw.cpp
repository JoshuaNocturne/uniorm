// The Oracle leg's native half: the same rows moved with OCI called by hand.
//
// Two shapes of write are timed here. One bind and one execute per row is what
// the plugin does, since it binds no arrays -- paired with the uniorm number of
// the same name, it says what the layer above the client costs. OCI's own array
// DML is what a client written against this library would do instead, so the
// three of those cases stand on their own in the table: no path through the
// plugin reaches them. Not every client accepts an array bind, and where one
// does not, those three ask once and stay out of the table rather than ending
// the leg.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <oci.h>

#include "perf_common.hpp"

using namespace uniorm;

namespace {

int const k_runs = 3;
std::size_t const k_batch = 1000;
// The width the table's two text columns were made with.
std::size_t const k_column_width = 64;

[[noreturn]] void native_fail(
  std::string const& context, OCIError* error, sword status) {
  char message[512] = {};
  sb4 code = 0;
  if (error == nullptr ||
      OCIErrorGet(error, 1, nullptr, &code,
        reinterpret_cast<OraText*>(message), sizeof(message),
        OCI_HTYPE_ERROR) != OCI_SUCCESS) {
    perf::fail("raw OCI " + context + ": status " +
      std::to_string(static_cast<int>(status)));
  }
  char label[32] = {};
  std::snprintf(label, sizeof(label), "ORA-%05u", static_cast<unsigned>(code));
  perf::fail("raw OCI " + context + ": " + label + " " + std::string(message));
}

void check(sword status, OCIError* error, std::string const& what) {
  if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
    native_fail(what, error, status);
  }
}

OraText const* ora_text(std::string const& value) {
  return reinterpret_cast<OraText const*>(value.data());
}

ub4 text_length(std::string const& value) {
  return static_cast<ub4>(value.size());
}

// The plugin creates its environment pinned to a pair of charsets it looks up
// rather than guesses, so this half pins to the same pair: a name that costs
// one byte on one side of the comparison costs it on the other.
void charset_ids(ub2& declared, ub2& national) {
  OCIEnv* throwaway = nullptr;
  if (OCIEnvCreate(&throwaway, OCI_THREADED, nullptr, nullptr, nullptr,
        nullptr, 0, nullptr) != OCI_SUCCESS) {
    perf::fail("raw OCI create an environment to look up charset ids");
  }
  auto const id = [throwaway](char const* name) {
    return OCINlsCharSetNameToId(
      throwaway, reinterpret_cast<OraText const*>(name));
  };
  ub2 const utf8 = id("AL32UTF8");
  ub2 const utf16 = id("AL16UTF16");
  OCIHandleFree(throwaway, OCI_HTYPE_ENV);
  if (utf8 == 0 || utf16 == 0) {
    perf::fail("raw OCI look up the AL32UTF8 and AL16UTF16 charset ids");
  }
  declared = utf8;
  national = utf16;
}

void number_from_integer(
  OCIError* error, std::int64_t whole, OCINumber& number) {
  sb8 value = static_cast<sb8>(whole);
  check(OCINumberFromInt(error, &value, sizeof(value), OCI_NUMBER_SIGNED,
           &number),
    error, "stage an integer as a NUMBER");
}

std::int64_t number_to_integer(OCIError* error, OCINumber const& number) {
  sb8 whole = 0;
  check(OCINumberToInt(error, &number, sizeof(whole), OCI_NUMBER_SIGNED,
           &whole),
    error, "read a NUMBER as an integer");
  return static_cast<std::int64_t>(whole);
}

struct native_session {
  OCIEnv* env = nullptr;
  OCIError* error = nullptr;
  OCISvcCtx* service = nullptr;

  explicit native_session(perf::leg_face const& face) {
    ub2 declared = 0;
    ub2 national = 0;
    charset_ids(declared, national);
    check(OCIEnvNlsCreate(&env, OCI_THREADED | OCI_OBJECT, nullptr, nullptr,
             nullptr, nullptr, 0, nullptr, declared, national),
      nullptr, "create the environment");
    check(OCIHandleAlloc(env, reinterpret_cast<void**>(&error),
             OCI_HTYPE_ERROR, 0, nullptr),
      nullptr, "allocate the diagnostics handle");
    sword const status = OCILogon2(env, error, &service, ora_text(face.user),
      text_length(face.user), ora_text(face.password),
      text_length(face.password), ora_text(face.target),
      text_length(face.target),
      face.user.empty() ? OCI_CRED_EXT : OCI_DEFAULT);
    if (status != OCI_SUCCESS) {
      native_fail("log on to " + face.target, error, status);
    }
  }

  ~native_session() {
    if (service != nullptr) {
      OCILogoff(service, error);
    }
    if (error != nullptr) {
      OCIHandleFree(error, OCI_HTYPE_ERROR);
    }
    if (env != nullptr) {
      OCIHandleFree(env, OCI_HTYPE_ENV);
    }
  }

  void commit(std::string const& what) {
    check(OCITransCommit(service, error, OCI_DEFAULT), error, what);
  }
};

// One column of rows staged as Oracle holds a NUMBER: the server's own binary
// form, with the per-row null flag and byte length beside it.
struct number_stage {
  std::vector<OCINumber> values;
  std::vector<sb2> nulls;
  std::vector<ub2> lengths;
  std::size_t rows = 0;

  explicit number_stage(std::size_t count)
    : values(count), nulls(count), lengths(count), rows(count) {}
};

// The same for a text column: the rows end to end in slots of the column's
// width, which is what the length array then reads down.
struct text_stage {
  std::vector<char> bytes;
  std::vector<sb2> nulls;
  std::vector<ub2> lengths;
  std::size_t rows = 0;

  explicit text_stage(std::size_t count)
    : bytes(count * k_column_width), nulls(count), lengths(count),
      rows(count) {}

  char* slot(std::size_t row) {
    return &bytes[row * k_column_width];
  }
};

// The four columns of one row, in whichever of the two stagings each belongs
// to: one batch's worth for array DML, or a single element for the per-row
// binds.
struct column_stages {
  number_stage ids;
  number_stage scores;
  text_stage names;
  text_stage notes;

  explicit column_stages(std::size_t count)
    : ids(count), scores(count), names(count), notes(count) {}

  void stage(std::size_t slot, Bench const& source, int score_shift,
    native_session& session) {
    number_from_integer(session.error, source.id, ids.values[slot]);
    ids.nulls[slot] = 0;
    number_from_integer(session.error,
      static_cast<std::int64_t>(source.score) + score_shift,
      scores.values[slot]);
    scores.nulls[slot] = 0;
    copy_text(slot, source.name, names, session);
    if (source.note) {
      copy_text(slot, *source.note, notes, session);
    } else {
      notes.nulls[slot] = -1;
      notes.lengths[slot] = 0;
    }
  }

private:
  void copy_text(std::size_t slot, std::string const& value,
    text_stage& stage, native_session& session) {
    if (value.size() > k_column_width) {
      perf::fail("raw OCI stage a " + std::to_string(value.size()) +
        "-byte value in a " + std::to_string(k_column_width) +
        "-byte column");
    }
    std::memcpy(stage.slot(slot), value.data(), value.size());
    stage.nulls[slot] = 0;
    stage.lengths[slot] = static_cast<ub2>(value.size());
  }
};

struct native_statement {
  native_session& session;
  OCIStmt* stmt = nullptr;
  std::vector<OCIBind*> binds;
  std::vector<OCIDefine*> defines;

  native_statement(native_session& owner, std::string const& sql)
    : session(owner) {
    check(OCIHandleAlloc(owner.env, reinterpret_cast<void**>(&stmt),
             OCI_HTYPE_STMT, 0, nullptr),
      owner.error, "allocate a statement");
    check(OCIStmtPrepare(stmt, owner.error, ora_text(sql), text_length(sql),
             OCI_NTV_SYNTAX, OCI_DEFAULT),
      owner.error, "prepare " + sql);
  }

  ~native_statement() {
    clear_binds();
    for (OCIDefine* handle : defines) {
      OCIHandleFree(handle, OCI_HTYPE_DEFINE);
    }
    if (stmt != nullptr) {
      OCIHandleFree(stmt, OCI_HTYPE_STMT);
    }
  }

  native_statement(native_statement const&) = delete;
  native_statement& operator=(native_statement const&) = delete;

  void clear_binds() {
    for (OCIBind* handle : binds) {
      OCIHandleFree(handle, OCI_HTYPE_BIND);
    }
    binds.clear();
  }

  // A parameter for `count` rows of the stage at once: the element widths are
  // the strides that turn one bind into an array of them. The count is asked
  // for rather than read off the staging's capacity, because a bind sized to
  // the whole buffer promises rows the execute never sends.
  void bind_numbers(std::size_t position, number_stage& stage,
    std::size_t count, std::string const& what) {
    OCIBind* handle = nullptr;
    check(OCIBindByPos(stmt, &handle, session.error,
             static_cast<ub4>(position), stage.values.data(),
             sizeof(OCINumber), SQLT_VNU, stage.nulls.data(), nullptr,
             nullptr, static_cast<ub4>(count), nullptr, OCI_DEFAULT),
      session.error, what);
    binds.push_back(handle);
    check(OCIBindArrayOfStruct(handle, session.error, sizeof(OCINumber),
             sizeof(sb2), 0, 0),
      session.error, "array-bind " + what);
  }

  void bind_text(std::size_t position, text_stage& stage,
    std::size_t count, std::string const& what) {
    OCIBind* handle = nullptr;
    check(OCIBindByPos(stmt, &handle, session.error,
             static_cast<ub4>(position), stage.bytes.data(),
             static_cast<sb4>(k_column_width), SQLT_CHR, stage.nulls.data(),
             stage.lengths.data(), nullptr,
             static_cast<ub4>(count), nullptr, OCI_DEFAULT),
      session.error, what);
    binds.push_back(handle);
    check(OCIBindArrayOfStruct(handle, session.error,
             static_cast<ub4>(k_column_width), sizeof(sb2), sizeof(ub2), 0),
      session.error, "array-bind " + what);
  }

  // One row's parameter, bound again for the next: the length it carries is
  // the row's own, so a reused statement takes a fresh bind per row.
  void bind_one_number(std::size_t position, OCINumber& number,
    std::string const& what) {
    OCIBind* handle = nullptr;
    check(OCIBindByPos(stmt, &handle, session.error,
             static_cast<ub4>(position), &number, sizeof(OCINumber), SQLT_VNU,
             nullptr, nullptr, nullptr, 0, nullptr, OCI_DEFAULT),
      session.error, what);
    binds.push_back(handle);
  }

  void bind_one_text(std::size_t position, text_stage& stage,
    std::string const& what) {
    OCIBind* handle = nullptr;
    check(OCIBindByPos(stmt, &handle, session.error,
             static_cast<ub4>(position), stage.slot(0),
             static_cast<sb4>(stage.lengths[0]), SQLT_CHR, &stage.nulls[0],
             nullptr, nullptr, 0, nullptr, OCI_DEFAULT),
      session.error, what);
    binds.push_back(handle);
  }

  void define_numbers(std::size_t position, number_stage& stage,
    std::string const& what) {
    OCIDefine* handle = nullptr;
    check(OCIDefineByPos(stmt, &handle, session.error,
             static_cast<ub4>(position), stage.values.data(),
             sizeof(OCINumber), SQLT_VNU, stage.nulls.data(),
             stage.lengths.data(), nullptr, OCI_DEFAULT),
      session.error, what);
    defines.push_back(handle);
    check(OCIDefineArrayOfStruct(handle, session.error, sizeof(OCINumber),
             sizeof(sb2), 0, 0),
      session.error, "array-define " + what);
  }

  void define_text(std::size_t position, text_stage& stage,
    std::string const& what) {
    OCIDefine* handle = nullptr;
    check(OCIDefineByPos(stmt, &handle, session.error,
             static_cast<ub4>(position), stage.bytes.data(),
             static_cast<sb4>(k_column_width), SQLT_CHR, stage.nulls.data(),
             stage.lengths.data(), nullptr, OCI_DEFAULT),
      session.error, what);
    defines.push_back(handle);
    check(OCIDefineArrayOfStruct(handle, session.error,
             static_cast<ub4>(k_column_width), sizeof(sb2), sizeof(ub2), 0),
      session.error, "array-define " + what);
  }

  // Rows the server hands over per round trip, which the plugin never asks
  // for and a client written against the library does.
  void set_prefetch(std::size_t rows, std::string const& what) {
    ub4 count = static_cast<ub4>(rows);
    check(OCIAttrSet(stmt, OCI_HTYPE_STMT, &count, sizeof(count),
             OCI_ATTR_PREFETCH_ROWS, session.error),
      session.error, what);
  }

  std::size_t execute(ub4 iterations, std::string const& what,
    ub4 mode = OCI_DEFAULT) {
    check(OCIStmtExecute(session.service, stmt, session.error, iterations, 0,
             nullptr, nullptr, mode),
      session.error, what);
    return attribute(OCI_ATTR_ROW_COUNT, what);
  }

  sword fetch(std::size_t rows) {
    return OCIStmtFetch2(stmt, session.error, static_cast<ub4>(rows),
      OCI_FETCH_NEXT, 0, OCI_DEFAULT);
  }

  std::size_t block_rows() {
    return attribute(OCI_ATTR_ROWS_FETCHED, "count the block");
  }

private:
  std::size_t attribute(ub4 id, std::string const& what) {
    ub4 count = 0;
    if (OCIAttrGet(stmt, OCI_HTYPE_STMT, &count, nullptr, id, session.error)
        != OCI_SUCCESS) {
      native_fail("read " + what, session.error, OCI_ERROR);
    }
    return static_cast<std::size_t>(count);
  }
};

// One autocommit statement: OCI has no autocommit attribute to set, so the
// mode on the execute is what commits the row.
void run_update(native_session& session, std::string const& sql,
  std::string const& what) {
  native_statement statement(session, sql);
  statement.execute(1, what, OCI_COMMIT_ON_SUCCESS);
}

std::int64_t count_rows(native_session& session, std::string const& table) {
  native_statement total(session, "SELECT count(*) FROM " + table);
  number_stage one(1);
  total.define_numbers(1, one, "define the row count");
  total.execute(0, "count the rows");
  sword const status = total.fetch(1);
  if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
    native_fail("read the row count", session.error, status);
  }
  return number_to_integer(session.error, one.values[0]);
}

// Some client and server pairs refuse an array bind outright: the 21c client
// this leg was measured with answers every array bind, down to one NUMBER
// column with no indicator, with ORA-03101. The array cases therefore ask
// first and stand aside rather than ending the leg, and they ask with a delete
// of an id no row can have, so the asking changes nothing.
bool array_dml_supported(native_session& session, std::string const& table) {
  std::string const sql = "DELETE FROM " + table + " WHERE id = :1";
  OCIStmt* stmt = nullptr;
  check(OCIHandleAlloc(session.env, reinterpret_cast<void**>(&stmt),
             OCI_HTYPE_STMT, 0, nullptr),
    session.error, "allocate the array probe");
  check(OCIStmtPrepare(stmt, session.error, ora_text(sql), text_length(sql),
             OCI_NTV_SYNTAX, OCI_DEFAULT),
    session.error, "prepare the array probe");
  number_stage one(1);
  number_from_integer(session.error, -1, one.values[0]);
  OCIBind* bind = nullptr;
  check(OCIBindByPos(stmt, &bind, session.error, 1, one.values.data(),
           sizeof(OCINumber), SQLT_VNU, one.nulls.data(), nullptr, nullptr,
           1, nullptr, OCI_DEFAULT),
    session.error, "bind the array probe");
  check(OCIBindArrayOfStruct(bind, session.error, sizeof(OCINumber),
           sizeof(sb2), 0, 0),
    session.error, "array-bind the probe");
  sword const status = OCIStmtExecute(session.service, stmt, session.error, 1,
    0, nullptr, nullptr, OCI_DEFAULT);
  bool const supported = status == OCI_SUCCESS ||
    status == OCI_SUCCESS_WITH_INFO;
  if (!supported) {
    char message[256] = {};
    sb4 code = 0;
    OCIErrorGet(session.error, 1, nullptr, &code,
      reinterpret_cast<OraText*>(message), sizeof(message), OCI_HTYPE_ERROR);
    std::size_t const last = std::strlen(message);
    for (std::size_t index = last; index > 0; --index) {
      if (message[index - 1] != '\n' && message[index - 1] != '\r') {
        message[index] = '\0';
        break;
      }
    }
    std::printf("%s: the array cases are skipped\n", message);
  }
  OCIHandleFree(bind, OCI_HTYPE_BIND);
  OCIHandleFree(stmt, OCI_HTYPE_STMT);
  return supported;
}

// The even ids, back in the table one row at a time: the state a delete pass
// took out, put between passes that cannot be timed against an emptied table.
void write_rows(native_session& session, std::string const& insert_sql,
  std::vector<Bench> const& rows) {
  native_statement ins(session, insert_sql);
  column_stages params(1);
  for (auto const& entry : rows) {
    ins.clear_binds();
    params.stage(0, entry, 0, session);
    ins.bind_one_number(1, params.ids.values[0], "bind the id to write back");
    ins.bind_one_text(2, params.names, "bind the name to write back");
    ins.bind_one_number(3, params.scores.values[0],
      "bind the score to write back");
    ins.bind_one_text(4, params.notes, "bind the note to write back");
    ins.execute(1, "write a row back");
  }
  session.commit("commit the rows written back");
}

void bind_row_params(native_statement& stmt, column_stages& params) {
  stmt.clear_binds();
  stmt.bind_one_number(1, params.ids.values[0], "bind the row id");
  stmt.bind_one_text(2, params.names, "bind the row name");
  stmt.bind_one_number(3, params.scores.values[0], "bind the row score");
  stmt.bind_one_text(4, params.notes, "bind the row note");
}

void bind_batch(native_statement& stmt, column_stages& params,
  std::size_t count) {
  stmt.clear_binds();
  stmt.bind_numbers(1, params.ids, count, "the batch id");
  stmt.bind_text(2, params.names, count, "the batch name");
  stmt.bind_numbers(3, params.scores, count, "the batch score");
  stmt.bind_text(4, params.notes, count, "the batch note");
}

// Every row the read brought back, in the shape the table holds it. A field
// that came over truncated would still count as a row, so the names say for
// themselves whether the staging was wide enough.
void check_assembled(std::vector<Bench> const& entities) {
  for (auto const& entry : entities) {
    bool const written = entry.id % 4 != 0;
    if (entry.name != "row-" + std::to_string(entry.id) ||
        !entry.note != !written ||
        (entry.note && *entry.note != "note-" + std::to_string(entry.id))) {
      perf::fail("raw read brought back a row that is not the one written");
    }
  }
}

}  // namespace

std::vector<perf::bench_result> run_raw_benchmarks(
  perf::leg_face const& face, std::size_t rows) {
  std::vector<perf::bench_result> results;
  std::string const table = perf::table_name();
  std::string const insert_sql = "INSERT INTO " + table +
    " (id, name, score, note) VALUES (:1, :2, :3, :4)";
  std::printf("\n[raw oracle oci baseline]\n");
  std::printf("%-34s %12s %14s\n", "benchmark", "time", "throughput");

  native_session session(face);
  std::vector<Bench> source = perf::make_rows(rows);
  std::vector<Bench> even;
  for (std::size_t index = 0; index < rows; index += 2) {
    even.push_back(source[index]);
  }

  // One bind and one execute per row in one transaction: the calls the
  // plugin's own batch makes, with nothing layered above them.
  std::size_t inserted = 0;
  {
    native_statement ins(session, insert_sql);
    column_stages params(1);
    perf::report("insert (batch)", rows,
      perf::best_of(
        [&] {
          run_update(session, "DELETE FROM " + table,
            "clear for the raw insert");
        },
        [&] {
          inserted = 0;
          for (auto const& entry : source) {
            params.stage(0, entry, 0, session);
            bind_row_params(ins, params);
            inserted += ins.execute(1, "execute the row insert");
          }
          session.commit("commit the raw insert");
        },
        k_runs), &results);
  }
  if (inserted != rows) {
    perf::fail("raw insert wrote " + std::to_string(inserted) + " of " +
      std::to_string(rows) + " rows");
  }
  if (count_rows(session, table) != static_cast<std::int64_t>(rows)) {
    perf::fail("the table does not hold the rows the raw insert wrote");
  }

  // What a client written against this library would do instead: the rows of
  // a batch bound as arrays and run by one execute.
  bool const arrays_ok = array_dml_supported(session, table);
  if (arrays_ok) {
    native_statement ins(session, insert_sql);
    column_stages params(k_batch);
    std::size_t batched = 0;
    perf::report("insert (array DML)", rows,
      perf::best_of(
        [&] {
          run_update(session, "DELETE FROM " + table,
            "clear for the array insert");
        },
        [&] {
          batched = 0;
          for (std::size_t first = 0; first < rows; first += k_batch) {
            std::size_t const count = std::min(k_batch, rows - first);
            for (std::size_t index = 0; index < count; ++index) {
              params.stage(index, source[first + index], 0, session);
            }
            bind_batch(ins, params, count);
            batched += ins.execute(static_cast<ub4>(count),
              "execute the array insert");
          }
          session.commit("commit the array insert");
        },
        1), &results);
    if (batched != rows) {
      perf::fail("the array insert wrote " + std::to_string(batched) +
        " of " + std::to_string(rows) + " rows");
    }
  }

  std::size_t updated = 0;
  {
    native_statement upd(session,
      "UPDATE " + table + " SET score = :1 WHERE id = :2");
    column_stages params(1);
    perf::report("update (batch)", rows,
      perf::best_of(
        [&] {
          updated = 0;
          for (auto const& entry : source) {
            upd.clear_binds();
            params.stage(0, entry, 1, session);
            upd.bind_one_number(1, params.scores.values[0],
              "bind the updated score");
            upd.bind_one_number(2, params.ids.values[0],
              "bind the updated id");
            updated += upd.execute(1, "execute the row update");
          }
          session.commit("commit the raw update");
        },
        k_runs), &results);
  }
  if (updated != rows) {
    perf::fail("raw update reached " + std::to_string(updated) + " of " +
      std::to_string(rows) + " rows");
  }

  if (arrays_ok) {
    native_statement upd(session,
      "UPDATE " + table + " SET score = :1 WHERE id = :2");
    column_stages params(k_batch);
    std::size_t batch_updated = 0;
    perf::report("update (array DML)", rows,
      perf::best_of(
        [&] {
          batch_updated = 0;
          for (std::size_t first = 0; first < rows; first += k_batch) {
            std::size_t const count = std::min(k_batch, rows - first);
            for (std::size_t index = 0; index < count; ++index) {
              params.stage(index, source[first + index], 1, session);
            }
            upd.clear_binds();
            upd.bind_numbers(1, params.scores, count,
              "the batch score to update");
            upd.bind_numbers(2, params.ids, count, "the batch id to update");
            batch_updated += upd.execute(static_cast<ub4>(count),
              "execute the array update");
          }
          session.commit("commit the array update");
        },
        1), &results);
    if (batch_updated != rows) {
      perf::fail("the array update reached " +
        std::to_string(batch_updated) + " of " + std::to_string(rows) +
        " rows");
    }
  }

  std::size_t const delete_count = even.size();
  std::size_t deleted = 0;
  {
    native_statement del(session, "DELETE FROM " + table + " WHERE id = :1");
    column_stages params(1);
    // A pass takes the even rows out, so they go back between passes, untimed:
    // timing the same request against an emptied table would report a speed
    // that pass never had.
    perf::perf_clock::duration best = perf::perf_clock::duration::max();
    for (int pass = 0; pass < k_runs; ++pass) {
      if (pass > 0) {
        write_rows(session, insert_sql, even);
      }
      auto const start = perf::perf_clock::now();
      deleted = 0;
      for (auto const& entry : even) {
        del.clear_binds();
        params.stage(0, entry, 0, session);
        del.bind_one_number(1, params.ids.values[0], "bind the id to delete");
        deleted += del.execute(1, "execute the row delete");
      }
      session.commit("commit the raw delete");
      auto const elapsed = perf::perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    perf::report("delete (batch)", delete_count, best, &results);
  }
  if (deleted != delete_count) {
    perf::fail("raw delete removed " + std::to_string(deleted) + " of " +
      std::to_string(delete_count) + " rows");
  }

  write_rows(session, insert_sql, even);
  if (arrays_ok) {
    native_statement del(session, "DELETE FROM " + table + " WHERE id = :1");
    column_stages params(k_batch);
    std::size_t batch_deleted = 0;
    perf::report("delete (array DML)", delete_count,
      perf::best_of(
        [&] {
          batch_deleted = 0;
          for (std::size_t first = 0; first < delete_count; first += k_batch) {
            std::size_t const count =
              std::min(k_batch, delete_count - first);
            for (std::size_t index = 0; index < count; ++index) {
              params.stage(index, even[first + index], 0, session);
            }
            del.clear_binds();
            del.bind_numbers(1, params.ids, count, "the batch id to delete");
            batch_deleted += del.execute(static_cast<ub4>(count),
              "execute the array delete");
          }
          session.commit("commit the array delete");
        },
        1), &results);
    if (batch_deleted != delete_count) {
      perf::fail("the array delete removed " +
        std::to_string(batch_deleted) + " of " +
        std::to_string(delete_count) + " rows");
    }
    // The rows that delete took out go back here, inside the same guard: an
    // array delete that never ran has left nothing to put back, and writing
    // them anyway would meet the ids that are already there.
    write_rows(session, insert_sql, even);
  }

  if (count_rows(session, table) != static_cast<std::int64_t>(rows)) {
    perf::fail("the table does not hold the rows the reads expect");
  }

  std::string const select_all =
    "SELECT id, name, score, note FROM " + table;

  // Block fetch through the server's own staging, nothing assembled out of
  // it: what the transport alone costs.
  std::size_t block_total = 0;
  {
    native_statement query(session, select_all);
    column_stages fetched(k_batch);
    query.define_numbers(1, fetched.ids, "define the fetched id");
    query.define_text(2, fetched.names, "define the fetched name");
    query.define_numbers(3, fetched.scores, "define the fetched score");
    query.define_text(4, fetched.notes, "define the fetched note");
    query.set_prefetch(k_batch, "prefetch the fetched rows");
    perf::report("query entity all (fetch only)", rows,
      perf::best_of(
        [&] {
          query.execute(0, "open the raw block read");
          block_total = 0;
          for (;;) {
            sword const status = query.fetch(k_batch);
            std::size_t const count = query.block_rows();
            if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO &&
                status != OCI_NO_DATA) {
              native_fail("fetch a block", session.error, status);
            }
            block_total += count;
            if (status == OCI_NO_DATA || count == 0) {
              break;
            }
          }
        },
        k_runs), &results);
  }
  if (block_total != rows) {
    perf::fail("raw block fetch returned " + std::to_string(block_total) +
      " of " + std::to_string(rows) + " rows");
  }

  // The same blocks, with the entities assembled out of them -- the fair
  // lower bound for what the library's entity path is asked to produce.
  std::size_t assembled = 0;
  {
    native_statement query(session, select_all);
    column_stages fetched(k_batch);
    query.define_numbers(1, fetched.ids, "define the entity id");
    query.define_text(2, fetched.names, "define the entity name");
    query.define_numbers(3, fetched.scores, "define the entity score");
    query.define_text(4, fetched.notes, "define the entity note");
    query.set_prefetch(k_batch, "prefetch the entity rows");
    std::vector<Bench> entities;
    perf::report("query entity all() (direct bind)", rows,
      perf::best_of(
        [&] {
          query.execute(0, "open the raw entity read");
          entities.clear();
          entities.reserve(rows);
          for (;;) {
            sword const status = query.fetch(k_batch);
            std::size_t const count = query.block_rows();
            if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO &&
                status != OCI_NO_DATA) {
              native_fail("fetch a block of entities", session.error, status);
            }
            for (std::size_t index = 0; index < count; ++index) {
              Bench row;
              row.id = number_to_integer(
                session.error, fetched.ids.values[index]);
              row.name.assign(fetched.names.slot(index),
                fetched.names.lengths[index]);
              row.score = static_cast<std::int32_t>(number_to_integer(
                session.error, fetched.scores.values[index]));
              if (fetched.notes.nulls[index] != -1) {
                row.note = std::string(fetched.notes.slot(index),
                  fetched.notes.lengths[index]);
              }
              entities.push_back(std::move(row));
            }
            if (status == OCI_NO_DATA || count == 0) {
              break;
            }
          }
          assembled = entities.size();
        },
        k_runs), &results);
    check_assembled(entities);
  }
  if (assembled != rows) {
    perf::fail("raw assemble returned " + std::to_string(assembled) +
      " of " + std::to_string(rows) + " rows");
  }

  std::string const one_row = select_all + " OFFSET 0 ROWS FETCH NEXT 1 "
    "ROWS ONLY";
  {
    native_statement query(session, one_row);
    column_stages fetched(1);
    query.define_numbers(1, fetched.ids, "define the single id");
    query.define_text(2, fetched.names, "define the single name");
    query.define_numbers(3, fetched.scores, "define the single score");
    query.define_text(4, fetched.notes, "define the single note");
    perf::report("query one() (direct bind)", 1,
      perf::best_of(
        [&] {
          query.execute(0, "open the raw single read");
          sword const status = query.fetch(1);
          if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
            native_fail("fetch the single row", session.error, status);
          }
          if (query.block_rows() != 1) {
            perf::fail("raw one() answered with no row");
          }
          Bench row;
          row.id = number_to_integer(session.error, fetched.ids.values[0]);
          row.name.assign(fetched.names.slot(0), fetched.names.lengths[0]);
          if (fetched.notes.nulls[0] != -1) {
            row.note = std::string(fetched.notes.slot(0),
              fetched.notes.lengths[0]);
          }
          // Which row a select list with no order answers with is the
          // server's, so the check is that the one it picked is itself.
          if (row.name != "row-" + std::to_string(row.id)) {
            perf::fail("raw one() answered with " + row.name + " for id " +
              std::to_string(row.id));
          }
        },
        k_runs), &results);
  }

  {
    native_statement total(session, "SELECT count(*) FROM " + table);
    number_stage one(1);
    total.define_numbers(1, one, "define the counted number");
    perf::report("query count()", 1,
      perf::best_of(
        [&] {
          total.execute(0, "open the raw count");
          sword const status = total.fetch(1);
          if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
            native_fail("fetch the count", session.error, status);
          }
          if (number_to_integer(session.error, one.values[0]) !=
              static_cast<std::int64_t>(rows)) {
            perf::fail("raw count() answered a different table");
          }
        },
        k_runs), &results);
  }
  return results;
}
