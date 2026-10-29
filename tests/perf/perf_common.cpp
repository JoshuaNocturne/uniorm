#include "perf_common.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <utility>

#include <uniorm/backend/error.hpp>
#include <uniorm/builder/builder.hpp>
#include <uniorm/orm.hpp>

using namespace uniorm;

namespace uniorm::perf {
namespace {

int const k_runs = 3;

struct projection_row {
  std::int64_t id;
  std::string name;
  std::int32_t score;
  std::optional<std::string> note;
};

struct simple_row {
  std::int64_t id;
  std::int32_t score;
};

struct conv_projection {
  std::int64_t id;
  std::string name;
  std::int32_t score;
  std::optional<BenchNote> note;
};

// A registry is built fresh for each case that wants its own statement cache:
// the numbers a shared cache would mix are the ones this leg is measured on.
orm build_registry(std::string const& connect_string) {
  orm registry(connect_string);
  registry.map<Bench>(table_name())
    .primary_key("id", &Bench::id)
    .column("name", &Bench::name)
    .column("score", &Bench::score)
    .column("note", &Bench::note);
  registry.row_array_size(1000);
  return registry;
}

orm build_conv_registry(std::string const& connect_string) {
  orm registry(connect_string);
  registry.map<BenchConv>(table_name())
    .primary_key("id", &BenchConv::id)
    .column("name", &BenchConv::name)
    .column("score", &BenchConv::score)
    .column("note", &BenchConv::note);
  registry.row_array_size(1000);
  return registry;
}

std::vector<Bench> make_even_rows(std::size_t count) {
  std::vector<Bench> rows;
  for (std::size_t index = 0; index < count; index += 2) {
    Bench row;
    row.id = static_cast<std::int64_t>(index);
    row.name = "row-" + std::to_string(index);
    row.score = static_cast<std::int32_t>(index % 1000);
    if (index % 4 != 0) {
      row.note = "note-" + std::to_string(index);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace

char const* table_name() {
  return "uniorm_perf_bench";
}

void report(char const* name, std::size_t rows,
  perf_clock::duration elapsed, std::vector<bench_result>* collect) {
  double const ms = std::chrono::duration<double, std::milli>(elapsed).count();
  double const krows_per_s = static_cast<double>(rows) / ms;  // rows/ms
  std::printf("%-34s %10.2f ms %12.1f krows/s\n", name, ms, krows_per_s);
  if (collect != nullptr) {
    collect->push_back({ name, rows, ms });
  }
}

void print_comparison_table(std::vector<bench_result> const& orm_results,
  std::vector<bench_result> const& raw_results) {
  std::printf("\n%-28s %10s %12s %10s %12s %6s\n",
    "benchmark", "ORM (ms)", "ORM (k/s)", "Raw (ms)", "Raw (k/s)", "Ratio");
  std::printf("%-28s %10s %12s %10s %12s %6s\n",
    std::string(28, '-').c_str(), std::string(10, '-').c_str(),
    std::string(12, '-').c_str(), std::string(10, '-').c_str(),
    std::string(12, '-').c_str(), std::string(6, '-').c_str());

  for (auto const& entry : orm_results) {
    auto const paired = std::find_if(raw_results.begin(), raw_results.end(),
      [&](bench_result const& other) { return other.name == entry.name; });
    std::string name = entry.name;
    if (name.length() > 28) {
      name = name.substr(0, 25) + "...";
    }
    double const orm_krows = static_cast<double>(entry.rows) / entry.ms;
    if (paired == raw_results.end()) {
      std::printf("%-28s %8.2f ms %10.1f k/s %10s %12s %6s\n",
        name.c_str(), entry.ms, orm_krows, "-", "-", "-");
      continue;
    }
    double const raw_krows = static_cast<double>(paired->rows) / paired->ms;
    // Above one means the same work cost less through the library than
    // through the layer above it.
    double const ratio = paired->ms / entry.ms;
    std::printf("%-28s %8.2f ms %10.1f k/s %8.2f ms %10.1f k/s %5.2fx\n",
      name.c_str(), entry.ms, orm_krows, paired->ms, raw_krows, ratio);
  }

  for (auto const& entry : raw_results) {
    auto const paired = std::find_if(orm_results.begin(), orm_results.end(),
      [&](bench_result const& other) { return other.name == entry.name; });
    if (paired != orm_results.end()) {
      continue;
    }
    std::string name = entry.name;
    if (name.length() > 28) {
      name = name.substr(0, 25) + "...";
    }
    double const raw_krows = static_cast<double>(entry.rows) / entry.ms;
    std::printf("%-28s %10s %12s %8.2f ms %10.1f k/s %6s\n",
      name.c_str(), "-", "-", entry.ms, raw_krows, "-");
  }
}

std::vector<Bench> make_rows(std::size_t count) {
  std::vector<Bench> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    Bench row;
    row.id = static_cast<std::int64_t>(index);
    row.name = "row-" + std::to_string(index);
    row.score = static_cast<std::int32_t>(index % 1000);
    if (index % 4 != 0) {
      row.note = "note-" + std::to_string(index);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<BenchConv> make_conv_rows(std::size_t count) {
  std::vector<BenchConv> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    BenchConv row;
    row.id = static_cast<std::int64_t>(index);
    row.name = "row-" + std::to_string(index);
    row.score = static_cast<std::int32_t>(index % 1000);
    if (index % 4 != 0) {
      row.note = BenchNote{ "note-" + std::to_string(index) };
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string fold_label(leg_face const& face, std::string_view name) {
  return face.server_dialect.fold_identifier(name);
}

bool open_leg(std::string const& scheme, leg_face& face, std::size_t& rows) {
  char const* target_env = std::getenv("UNIORM_IT_DSN");
  char const* user_env = std::getenv("UNIORM_IT_USER");
  char const* password_env = std::getenv("UNIORM_IT_PWD");
  if (target_env == nullptr || !*target_env || user_env == nullptr ||
      !*user_env || password_env == nullptr || !*password_env) {
    std::printf("skip: set UNIORM_IT_DSN / UNIORM_IT_USER / UNIORM_IT_PWD to "
      "run the %s perf leg\n", scheme.c_str());
    return false;
  }
  face.scheme = scheme;
  face.target = target_env;
  face.user = user_env;
  face.password = password_env;
  face.id_type = "BIGINT";
  face.score_type = "INT";
  if (scheme == "odbc") {
    face.connect_string =
      "DSN=" + face.target + ";UID=" + face.user + ";PWD=" + face.password;
  } else if (scheme == "oracle") {
    face.connect_string =
      "oracle://" + face.user + "/" + face.password + "@" + face.target;
    // This server has no BIGINT and its DECIMAL stops at 38 digits, so the
    // two columns are spelled in its own words.
    face.id_type = "NUMBER(19)";
    face.score_type = "NUMBER(10)";
  } else if (scheme == "postgresql") {
    face.connect_string =
      "postgresql://" + face.user + ":" + face.password + "@" + face.target;
  } else {
    std::printf("skip: no perf leg drives scheme '%s'\n", scheme.c_str());
    return false;
  }

  try {
    connection probe(face.connect_string);
    std::printf("connected over '%s': dbms = %s\n", scheme.c_str(),
      probe.dbms_name().c_str());
    face.server_dialect = dialect::detect(probe.dbms_name());
  } catch (std::exception const& error) {
    std::printf("skip: cannot connect to '%s': %s\n", face.target.c_str(),
      error.what());
    return false;
  }
  rows = row_count();
  return true;
}

std::size_t row_count() {
  char const* rows_env = std::getenv("UNIORM_PERF_ROWS");
  std::size_t const count =
    (rows_env != nullptr && *rows_env) ? std::strtoul(rows_env, nullptr, 10)
                                       : 10000;
  return (count == 0 || count > 1'000'000) ? 10000 : count;
}

void fail(std::string const& context) {
  std::printf("FATAL: %s\n", context.c_str());
  std::fflush(stdout);
  std::exit(1);
}

std::vector<bench_result> run_orm_benchmarks(
  leg_face const& face, std::size_t rows) {
  std::vector<bench_result> results;
  connection conn(face.connect_string);

  conn.execute_update(std::string("DROP TABLE IF EXISTS ") + table_name());
  conn.execute_update(std::string("CREATE TABLE ") + table_name() +
    " (id " + face.id_type + " NOT NULL PRIMARY KEY,"
    " name VARCHAR(64) NOT NULL,"
    " score " + face.score_type + " NOT NULL,"
    " note VARCHAR(64) NULL)");

  orm registry = build_registry(face.connect_string);
  orm conv_registry = build_conv_registry(face.connect_string);
  // A backend that answers no catalog leaves the validation behind, and with
  // it the one case that needs the schema rather than the mapping.
  try {
    conn.schema();
    conv_registry.validate();
  } catch (backend::capability_not_supported const&) {
  }

  std::printf("rows per case: %zu (best of %d runs)\n", rows, k_runs);
  std::printf("\n[uniorm over the %s backend]\n", face.scheme.c_str());
  std::printf("%-34s %12s %14s\n", "benchmark", "time", "throughput");

  std::vector<Bench> source = make_rows(rows);

  {
    orm fresh = build_registry(face.connect_string);
    conn.execute_update(std::string("DELETE FROM ") + table_name());
    fresh.insert(source);
    fresh.clear_statement_cache();

    std::vector<Bench> updated = source;
    for (auto& row : updated) {
      row.score += 1;
    }
    auto start = perf_clock::now();
    fresh.update(updated);
    double const ms = std::chrono::duration<double, std::milli>(
      perf_clock::now() - start).count();
    std::printf("update (fresh orm, no prior cache)  %8.2f ms %12.1f krows/s\n",
      ms, static_cast<double>(rows) / ms);
  }

  std::size_t inserted = 0;
  {
    perf_clock::duration best = perf_clock::duration::max();
    for (int run = 0; run < k_runs; ++run) {
      conn.execute_update(std::string("DELETE FROM ") + table_name());
      auto start = perf_clock::now();
      inserted = registry.insert(source);
      auto elapsed = perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    report("insert (batch)", rows, best, &results);
  }
  if (inserted != rows) {
    fail("inserted " + std::to_string(inserted) + " of " +
      std::to_string(rows) + " rows");
  }

  std::size_t updated = 0;
  {
    registry.clear_statement_cache();
    perf_clock::duration best = perf_clock::duration::max();
    for (int run = 0; run < k_runs; ++run) {
      // The scores go back to what the table holds before the timed pass, so
      // every run updates the same number of rows to the same new values.
      if (run > 0) {
        registry.update(source);
      }
      std::vector<Bench> raised = source;
      for (auto& row : raised) {
        row.score += 1;
      }
      auto start = perf_clock::now();
      updated = registry.update(raised);
      auto elapsed = perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    report("update (batch)", rows, best, &results);
  }
  if (updated != rows) {
    fail("updated " + std::to_string(updated) + " of " +
      std::to_string(rows) + " rows");
  }

  std::vector<Bench> even = make_even_rows(rows);
  std::size_t deleted = 0;
  {
    perf_clock::duration best = perf_clock::duration::max();
    for (int run = 0; run < k_runs; ++run) {
      if (run > 0) {
        registry.insert(even);
      }
      auto start = perf_clock::now();
      deleted = registry.remove(even);
      auto elapsed = perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    report("delete (batch)", even.size(), best, &results);
  }
  if (deleted != even.size()) {
    fail("deleted " + std::to_string(deleted) + " of " +
      std::to_string(even.size()) + " rows");
  }
  registry.insert(even);

  std::string const select_all =
    std::string("SELECT id, name, score, note FROM ") + table_name();

  std::size_t entity_rows = 0;
  report("query entity all() (direct bind)", rows,
    best_of([&] {
      entity_rows = registry.query<Bench>().all().size();
    }, k_runs), &results);
  if (entity_rows != rows) {
    fail("entity query returned " + std::to_string(entity_rows) + " rows");
  }

  std::size_t projected = 0;
  report("query projection (with string)", rows,
    best_of([&] {
      projected = registry.query<projection_row>(select_all).size();
    }, k_runs), &results);
  if (projected != rows) {
    fail("projection returned " + std::to_string(projected) + " rows");
  }

  std::size_t simple_rows = 0;
  report("query projection (POD only)", rows,
    best_of([&] {
      simple_rows = registry.query<simple_row>(
        std::string("SELECT id, score FROM ") + table_name()).size();
    }, k_runs), &results);
  if (simple_rows != rows) {
    fail("POD projection returned " + std::to_string(simple_rows) + " rows");
  }

  std::string const id_label = fold_label(face, "id");
  std::size_t dynamic_rows = 0;
  report("query dynamic rows (row/sql_value)", rows,
    best_of([&] {
      result_set stream = conn.execute(select_all);
      dynamic_rows = 0;
      while (stream.next()) {
        row current = stream.current();
        current.get<std::int64_t>(id_label);
        ++dynamic_rows;
      }
    }, k_runs), &results);
  if (dynamic_rows != rows) {
    fail("dynamic query returned " + std::to_string(dynamic_rows) + " rows");
  }

  report("query one() (direct bind)", 1,
    best_of([&] {
      if (!registry.query<Bench>().limit(1).one()) {
        fail("one() answered with no row");
      }
    }, k_runs), &results);

  std::int64_t counted = 0;
  report("query count()", 1,
    best_of([&] { counted = registry.query<Bench>().count(); }, k_runs),
    &results);
  if (counted != static_cast<std::int64_t>(rows)) {
    fail("count() returned " + std::to_string(counted));
  }

  std::printf("\n[converter field vs plain field, same columns]\n");

  std::vector<BenchConv> conv_source = make_conv_rows(rows);
  std::size_t conv_inserted = 0;
  {
    perf_clock::duration best = perf_clock::duration::max();
    for (int run = 0; run < k_runs; ++run) {
      conn.execute_update(std::string("DELETE FROM ") + table_name());
      auto start = perf_clock::now();
      conv_inserted = conv_registry.insert(conv_source);
      auto elapsed = perf_clock::now() - start;
      if (elapsed < best) {
        best = elapsed;
      }
    }
    report("insert (batch, converter field)", rows, best, &results);
  }
  if (conv_inserted != rows) {
    fail("converter insert wrote " + std::to_string(conv_inserted) + " of " +
      std::to_string(rows) + " rows");
  }

  std::size_t conv_read = 0;
  report("query entity all (converter field)", rows,
    best_of([&] {
      conv_read = conv_registry.query<BenchConv>().all().size();
    }, k_runs), &results);
  if (conv_read != rows) {
    fail("converter entity query returned " + std::to_string(conv_read) +
      " rows");
  }
  // Untimed, and a read of its own: a decode that quietly produced nothing
  // would still have counted these rows above.
  for (auto const& entry : conv_registry.query<BenchConv>().all()) {
    bool const written = entry.id % 4 != 0;
    if (!entry.note != !written || (entry.note &&
        entry.note->text != "note-" + std::to_string(entry.id))) {
      fail("converter decode wrong at id " + std::to_string(entry.id));
    }
  }

  std::size_t conv_projected = 0;
  report("query projection (converter field)", rows,
    best_of([&] {
      conv_projected = registry.query<conv_projection>(select_all).size();
    }, k_runs), &results);
  if (conv_projected != rows) {
    fail("converter projection returned " + std::to_string(conv_projected) +
      " rows");
  }
  return results;
}

void drop_bench_table(std::string const& connect_string) {
  connection conn(connect_string);
  conn.execute_update(std::string("DROP TABLE ") + table_name());
}

}  // namespace uniorm::perf
