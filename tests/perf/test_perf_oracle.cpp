// The Oracle leg of the perf comparison: uniorm's own paths against OCI calls
// made by hand, both against the server UNIORM_IT_DSN names.
//
// The target, user and password come from UNIORM_IT_DSN / UNIORM_IT_USER /
// UNIORM_IT_PWD, the row count from UNIORM_PERF_ROWS (default 10000), and the
// DSN is an Oracle connect string in its own grammar. Any piece missing, or a
// server that does not answer, returns 77, which ctest reads as a skip. The
// core dlopens the plugin by scheme, so this target links nothing but the
// core; perf_oracle_raw.cpp is the half that names OCI.

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "perf_common.hpp"

// perf_oracle_raw.cpp
std::vector<uniorm::perf::bench_result> run_raw_benchmarks(
  uniorm::perf::leg_face const& face, std::size_t rows);

int main() {
  uniorm::perf::leg_face face;
  std::size_t rows = 0;
  if (!uniorm::perf::open_leg("oracle", face, rows)) {
    return 77;
  }
  try {
    auto orm_results = uniorm::perf::run_orm_benchmarks(face, rows);
    auto raw_results = run_raw_benchmarks(face, rows);
    uniorm::perf::print_comparison_table(orm_results, raw_results);
    uniorm::perf::drop_bench_table(face.connect_string);
  } catch (std::exception const& error) {
    std::printf("FATAL: unexpected exception: %s\n", error.what());
    return 1;
  }
  return 0;
}
