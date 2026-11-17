#include "probe_cache.hpp"

#include <mutex>
#include <unordered_map>

#include "identifier.hpp"

namespace uniorm::odbc {

namespace {

std::string version_text(version_triple version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor) +
         "." + std::to_string(version.patch);
}

struct cache_state {
  std::mutex mutex;
  std::unordered_map<std::string, probe_conclusion> entries;
};

cache_state& state() {
  static cache_state instance;
  return instance;
}

}  // namespace

std::string probe_cache_key(std::string_view driver_name,
  version_triple driver_ver, std::string_view dbms_name,
  version_triple dbms_ver) {
  return "driver=" + uniorm::detail::fold_lower(driver_name) +
         "|dver=" + version_text(driver_ver) +
         "|dbms=" + uniorm::detail::fold_lower(dbms_name) +
         "|sver=" + version_text(dbms_ver);
}

std::optional<probe_conclusion> resolve_probe(
  uniorm::detail::array_probe_result const& result) {
  using uniorm::detail::probe_outcome;
  if (result.insert == probe_outcome::inconclusive ||
      result.update == probe_outcome::inconclusive ||
      result.remove == probe_outcome::inconclusive) {
    return std::nullopt;
  }
  probe_conclusion conclusion;
  conclusion.array_insert = result.insert == probe_outcome::pass;
  conclusion.array_update = result.update == probe_outcome::pass;
  conclusion.array_delete = result.remove == probe_outcome::pass;
  conclusion.array_rowcount_totals = result.rowcount_totals;
  return conclusion;
}

bool probe_cache_lookup(std::string const& key, probe_conclusion& out) {
  auto& cache = state();
  std::lock_guard<std::mutex> lock(cache.mutex);
  auto found = cache.entries.find(key);
  if (found == cache.entries.end()) {
    return false;
  }
  out = found->second;
  return true;
}

void probe_cache_store(
  std::string const& key, probe_conclusion const& conclusion) {
  auto& cache = state();
  std::lock_guard<std::mutex> lock(cache.mutex);
  cache.entries[key] = conclusion;
}

probe_conclusion probe_unknown_combo(backend::connection_iface& conn,
  std::string_view driver_name, version_triple driver_ver,
  std::string_view dbms_name, version_triple dbms_ver) {
  std::string const key =
    probe_cache_key(driver_name, driver_ver, dbms_name, dbms_ver);
  probe_conclusion conclusion;
  if (probe_cache_lookup(key, conclusion)) {
    return conclusion;
  }
  uniorm::detail::array_probe_result const result =
    uniorm::detail::probe_array_dml(conn, 8);
  if (auto resolved = resolve_probe(result)) {
    probe_cache_store(key, *resolved);
    return *resolved;
  }
  return probe_conclusion{};
}

}  // namespace uniorm::odbc