#pragma once

// Public header: contract a backend shared library exports so the
// core's dlopen-based loader can call into it. See design.md §5.1.

#include <cstdint>

namespace uniorm::backend {

class registry;

// Names of the two extern "C" symbols a plugin must export.
inline constexpr const char* plugin_abi_symbol =
  "uniorm_plugin_abi_version";
inline constexpr const char* plugin_register_symbol =
  "uniorm_plugin_register";

// Signatures of those two symbols. A plugin's uniorm_plugin_abi_version
// must return UNIORM_ABI_VERSION, which the core exports as a PUBLIC
// compile-time definition derived from PROJECT_VERSION.
using plugin_abi_fn = uint32_t (*)();
using plugin_register_fn = void (*)(registry&);

}  // namespace uniorm::backend
