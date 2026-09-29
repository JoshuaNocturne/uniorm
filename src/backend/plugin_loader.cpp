#include "plugin_loader.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

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

#ifdef _WIN32
namespace {
std::string last_error_message() {
  DWORD error = GetLastError();
  if (error == 0) {
    return "(no error)";
  }
  char* buffer = nullptr;
  DWORD length = FormatMessageA(
    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
      FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return "(no message)";
  }
  std::string message(buffer, length);
  LocalFree(buffer);
  while (!message.empty() &&
         (message.back() == '\r' || message.back() == '\n' ||
          message.back() == ' ')) {
    message.pop_back();
  }
  return message;
}
}  // namespace
#endif

bool try_load_plugin(std::string_view scheme, std::string& error_note) {
  std::string dir = plugin_dir();
  if (dir.empty()) {
    error_note = "no plugin directory configured (set UNIORM_PLUGIN_PATH "
                 "or build with UNIORM_PLUGIN_DIR)";
    return false;
  }
  std::string path = dir;
  if (path.back() != '/' && path.back() != '\\') {
    path.push_back('/');
  }
#ifdef _WIN32
  path.append("uniorm_");
  path.append(scheme);
  path.append(".dll");

  HMODULE handle =
    LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (handle == nullptr) {
    error_note = "LoadLibraryEx(" + path + ") failed: " + last_error_message();
    return false;
  }

  auto abi_raw =
    reinterpret_cast<void*>(GetProcAddress(handle, plugin_abi_symbol));
  if (abi_raw == nullptr) {
    error_note = "GetProcAddress(" + path + ", " + plugin_abi_symbol +
                 ") failed: " + last_error_message();
    FreeLibrary(handle);
    return false;
  }
  const auto abi_fn = reinterpret_cast<plugin_abi_fn>(abi_raw);
  const uint32_t plugin_abi = abi_fn();
  if (plugin_abi != UNIORM_ABI_VERSION) {
    error_note = "plugin ABI " + std::to_string(plugin_abi) +
                 " incompatible with core ABI " +
                 std::to_string(UNIORM_ABI_VERSION) + " (" + path + ")";
    FreeLibrary(handle);
    return false;
  }

  auto reg_raw =
    reinterpret_cast<void*>(GetProcAddress(handle, plugin_register_symbol));
  if (reg_raw == nullptr) {
    error_note = "GetProcAddress(" + path + ", " + plugin_register_symbol +
                 ") failed: " + last_error_message();
    FreeLibrary(handle);
    return false;
  }
  const auto reg_fn = reinterpret_cast<plugin_register_fn>(reg_raw);
  reg_fn(registry::instance());
  return true;
#else
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
#endif
}

}  // namespace uniorm::backend::detail
