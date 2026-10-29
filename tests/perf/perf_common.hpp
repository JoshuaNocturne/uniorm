#pragma once

// What every perf leg shares: the row it moves, the timers, the table that
// puts a leg's uniorm numbers beside the numbers its own client library
// answers with, and the uniorm half of the comparison. That half lives here
// because one contract is driven by all three legs -- what a server spells
// differently is read from the face this run opened, never assumed from the
// leg that was written first. Each leg's native half is its own file, since
// nothing about SQLBindParameter, OCIBindByPos and PQexecPrepared is shared.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <uniorm/connection.hpp>
#include <uniorm/converter.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/mapping/registry.hpp>

namespace uniorm {

// The row every case moves: four columns, the last of them nullable.
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

// The same four columns with the nullable one behind that converter, so the
// cost of the extension point is measurable against the plain field beside it.
struct BenchConv {
  std::int64_t id = 0;
  std::string name;
  std::int32_t score = 0;
  std::optional<BenchNote> note;
};

namespace perf {

using perf_clock = std::chrono::steady_clock;

// The one table all three legs create, drop and read. Unquoted, so each
// server folds it the way it folds any name it is not handed in quotes.
char const* table_name();

// One timed case. A name is what lets the comparison table find the uniorm
// number and the native number for the same work, and the duration is one pass
// of `rows` rows however many passes were run to take the best of them.
struct bench_result {
  std::string name;
  std::size_t rows;
  double ms;
};

void report(char const* name, std::size_t rows, perf_clock::duration elapsed,
  std::vector<bench_result>* collect = nullptr);

template <class Fn>
perf_clock::duration best_of(Fn&& fn, int runs) {
  perf_clock::duration best = perf_clock::duration::max();
  for (int index = 0; index < runs; ++index) {
    auto start = perf_clock::now();
    fn();
    auto elapsed = perf_clock::now() - start;
    if (elapsed < best) {
      best = elapsed;
    }
  }
  return best;
}

// The same, with the work that puts the table back between runs kept out of
// the timed region. A case that deletes rows cannot be run twice on the state
// its first run left behind: the second pass would time a statement that had
// nothing to remove, and best-of would report that one.
template <class Setup, class Fn>
perf_clock::duration best_of(Setup&& setup, Fn&& fn, int runs) {
  perf_clock::duration best = perf_clock::duration::max();
  for (int index = 0; index < runs; ++index) {
    setup();
    auto start = perf_clock::now();
    fn();
    auto elapsed = perf_clock::now() - start;
    if (elapsed < best) {
      best = elapsed;
    }
  }
  return best;
}

void print_comparison_table(std::vector<bench_result> const& orm_results,
  std::vector<bench_result> const& raw_results);

// The rows a case writes. A note is absent on every fourth one, so the
// nullable path is walked by a quarter of the table's rows on every leg.
std::vector<Bench> make_rows(std::size_t count);
std::vector<BenchConv> make_conv_rows(std::size_t count);

// What this run's server and plugin spell differently.
struct leg_face {
  std::string scheme;          // the plugin this leg loads
  std::string connect_string;  // what a connection is handed
  std::string target;          // the target alone, in this scheme's grammar
  std::string user;
  std::string password;
  std::string id_type;         // this server's 64-bit integral column
  std::string score_type;      // and its 32-bit one
  dialect server_dialect;      // detected from the banner this run read
};

// The label a dynamic read asks for: this server's own spelling of a name
// written lower-case here.
std::string fold_label(leg_face const& face, std::string_view name);

// Reads the three variables, puts the target in this scheme's grammar and
// asks the server which it is. Prints the reason and returns false when a
// piece is missing, the scheme is not this one, or nothing answers.
bool open_leg(std::string const& scheme, leg_face& face, std::size_t& rows);

// The row count every case moves, from UNIORM_PERF_ROWS.
std::size_t row_count();

[[noreturn]] void fail(std::string const& context);

// The uniorm half, both registries included: writes, reads, and the
// converter's field against the plain one. It leaves the table in place, so
// a leg's native half can read what this wrote.
std::vector<bench_result> run_orm_benchmarks(
  leg_face const& face, std::size_t rows);

// Drops what prepare_bench_table made. A leg's main calls this last.
void drop_bench_table(std::string const& connect_string);

}  // namespace perf
}  // namespace uniorm
