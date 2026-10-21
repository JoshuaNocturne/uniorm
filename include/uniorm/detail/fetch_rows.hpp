#pragma once

// The block-fetch materialization every typed read shares. It lives apart from
// both orm and the builders so the synchronous path and the asynchronous one
// run one loop rather than two that drift.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/connection.hpp>
#include <uniorm/params.hpp>

namespace uniorm::detail {

// One describe round, taken by `binding` to place its columns, then every
// fetched row materialized straight into the vector's storage. A row that
// throws un-does its own partial rows; the statement then unwinds instead of
// returning to the cache, as it has always done.
template <class T, class Binding>
std::vector<T> fetch_rows(connection& conn, std::string_view sql,
  params const& bound, std::size_t row_array_size, Binding& binding) {
  std::string key(sql);
  auto stmt = conn.acquire_statement(key);
  stmt->bind_params(bound);
  stmt->execute();
  stmt->set_row_array_size(row_array_size);
  binding.set_row_array_size(row_array_size);
  binding.bind(*stmt);
  std::vector<T> out;
  if (std::size_t est = stmt->result_row_estimate()) out.reserve(est);
  while (stmt->fetch()) {
    std::size_t rows_fetched = stmt->rows_fetched();
    std::size_t old_size = out.size();
    out.resize(out.size() + rows_fetched);
    try {
      for (std::size_t index = 0; index < rows_fetched; ++index) {
        binding.fill_into(out[old_size + index], index);
      }
    } catch (...) {
      out.resize(old_size);
      throw;
    }
  }
  conn.release_statement(key, std::move(stmt));
  return out;
}

// The same read stopped at the first row: a one-row array, and the value moved
// out of the binding instead of into vector storage.
template <class T, class Binding>
std::optional<T> fetch_one(
  connection& conn, std::string_view sql, params const& bound,
  Binding& binding) {
  std::string key(sql);
  auto stmt = conn.acquire_statement(key);
  stmt->bind_params(bound);
  stmt->execute();
  stmt->set_row_array_size(1);
  binding.set_row_array_size(1);
  binding.bind(*stmt);
  std::optional<T> result;
  if (stmt->fetch()) {
    result = binding.take(0);
  }
  conn.release_statement(key, std::move(stmt));
  return result;
}

}  // namespace uniorm::detail
