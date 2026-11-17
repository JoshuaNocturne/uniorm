#pragma once

// Private header: a process-wide cache of effect-level probe verdicts for ODBC
// combos the compatibility table does not know. Not installed. Executability is
// a driver/server property, so the key is the identity, not the connection
// string; DDL privilege is per-connection, so an inconclusive probe is never
// cached as a global hard deny.

#include <optional>
#include <string>
#include <string_view>

#include <uniorm/backend/backend.hpp>

#include "array_probe.hpp"
#include "compat.hpp"
#include "export.hpp"

namespace uniorm::odbc {

// The four resolved bits. Defaults are conservative deny: a combo nothing has
// measured stays off the array path until a verdict says otherwise.
struct probe_conclusion {
  bool array_insert = false;
  bool array_update = false;
  bool array_delete = false;
  bool array_rowcount_totals = true;
};

UNIORM_ODBC_API std::string probe_cache_key(std::string_view driver_name,
  version_triple driver_ver, std::string_view dbms_name,
  version_triple dbms_ver);

// Resolves a probe result to bits, or nullopt when any op is inconclusive: a
// connection or scratch-DDL failure proves nothing about executability.
UNIORM_ODBC_API std::optional<probe_conclusion> resolve_probe(
  uniorm::detail::array_probe_result const& result);

UNIORM_ODBC_API bool probe_cache_lookup(
  std::string const& key, probe_conclusion& out);
UNIORM_ODBC_API void probe_cache_store(
  std::string const& key, probe_conclusion const& conclusion);

// The table-miss decision: a cached verdict if present, else one probe of this
// connection. A conclusive verdict is applied and cached; an inconclusive one
// leaves conservative deny and is not cached. The probe runs outside the cache
// lock, so two connections racing the same unknown combo may both probe once.
UNIORM_ODBC_API probe_conclusion probe_unknown_combo(
  backend::connection_iface& conn, std::string_view driver_name,
  version_triple driver_ver, std::string_view dbms_name,
  version_triple dbms_ver);

}  // namespace uniorm::odbc