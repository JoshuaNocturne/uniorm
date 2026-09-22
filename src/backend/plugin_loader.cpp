#include "plugin_loader.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <string>

#include "uniorm/backend/plugin.hpp"
#include "uniorm/backend/registry.hpp"

namespace uniorm::backend::detail {

std::string plugin_dir() {
  const char* env = std::getenv("UNIORM_PLUGIN_PATH");
  if (env != nullptr && *env != '\0') {
    return env;
  }
#ifdef UNIORM_PLUGIN_DIR
  return UNIORM_PLUGIN_DIR;
#else
  return {};
#endif
}

bool try_load_plugin(std::string_view scheme, std::string& error_note) {
  std::string dir = plugin_dir();
  if (dir.empty()) {
    error_note = "no plugin directory configured (set UNIORM_PLUGIN_PATH "
                 "or build with UNIORM_PLUGIN_DIR)";
    return false;
  }
  std::string path = dir;
  if (path.back() != '/') {
    path.push_back('/');
  }
  path.append("libuniorm_");
  path.append(scheme);
  path.append(".so");

  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    const char* err = dlerror();
    error_note = "dlopen(" + path + ") failed: ";
    error_note += err ? err : "(no message)";
    return false;
  }

  void* abi_raw = dlsym(handle, plugin_abi_symbol);
  if (abi_raw == nullptr) {
    const char* err = dlerror();
    error_note = "dlsym(" + path + ", " + plugin_abi_symbol + ") failed: ";
    error_note += err ? err : "(no message)";
    dlclose(handle);
    return false;
  }
  const auto abi_fn = reinterpret_cast<plugin_abi_fn>(abi_raw);
  const uint32_t plugin_abi = abi_fn();
  if (plugin_abi != UNIORM_ABI_VERSION) {
    error_note = "plugin ABI " + std::to_string(plugin_abi) +
                 " incompatible with core ABI " +
                 std::to_string(UNIORM_ABI_VERSION) + " (" + path + ")";
    dlclose(handle);
    return false;
  }

  void* reg_raw = dlsym(handle, plugin_register_symbol);
  if (reg_raw == nullptr) {
    const char* err = dlerror();
    error_note = "dlsym(" + path + ", " + plugin_register_symbol +
                 ") failed: ";
    error_note += err ? err : "(no message)";
    dlclose(handle);
    return false;
  }
  const auto reg_fn = reinterpret_cast<plugin_register_fn>(reg_raw);
  reg_fn(registry::instance());
  return true;
}

}  // namespace uniorm::backend::detail
