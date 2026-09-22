#pragma once

// Private header: dlopen-based loader for backend plugins. Not installed.

#include <string>
#include <string_view>

namespace uniorm::backend::detail {

// Effective plugin directory: UNIORM_PLUGIN_PATH when set and
// non-empty, else the UNIORM_PLUGIN_DIR compile-time constant baked
// in by CMake at configure time for top-level builds. Returns "" when
// neither is available.
std::string plugin_dir();

// dlopen the plugin that would serve `scheme`, verify its ABI, and
// hand the core's registry to its register entry. On failure,
// `error_note` says what was tried and why. The handle is intentionally
// not dlclosed on success: the plugin's vtables outlive this call.
bool try_load_plugin(std::string_view scheme, std::string& error_note);

}  // namespace uniorm::backend::detail
