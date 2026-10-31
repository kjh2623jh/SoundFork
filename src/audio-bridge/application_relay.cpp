#include "audio-bridge/application_relay.hpp"

#include <string>
#include <vector>

namespace soundfork {
namespace {

bool read_setting(const wchar_t* name, std::wstring* value) noexcept {
  value->clear();
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SoundFork", 0,
                    KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0;
  DWORD bytes = 0;
  auto result = RegQueryValueExW(
      key, name, nullptr, &type, nullptr, &bytes);
  if (result != ERROR_SUCCESS || type != REG_SZ ||
      bytes < sizeof(wchar_t) || bytes > 8192 ||
      bytes % sizeof(wchar_t) != 0) {
    RegCloseKey(key);
    return false;
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
  result = RegQueryValueExW(key, name, nullptr, &type,
      reinterpret_cast<BYTE*>(buffer.data()), &bytes);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS || buffer.empty() || buffer.back() != L'\0') {
    return false;
  }
  for (std::size_t index = 0; index + 1 < buffer.size(); ++index) {
    if (buffer[index] == L'\0') return false;
  }
  value->assign(buffer.data(), buffer.size() - 1);
  return true;
}

std::wstring module_directory() noexcept {
  static const wchar_t anchor = 0;
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          &anchor, &module)) {
    return {};
  }
  std::vector<wchar_t> path;
  try {
    path.resize(32768, L'\0');
  } catch (...) {
    return {};
  }
  const DWORD length = GetModuleFileNameW(
      module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return {};
  std::wstring result(path.data(), length);
  const auto separator = result.find_last_of(L"\\/");
  if (separator == std::wstring::npos) return {};
  result.resize(separator);
  return result;
}

}  // namespace

ApplicationRelay::~ApplicationRelay() {
  stop();
}

void ApplicationRelay::start_if_enabled() noexcept {
  if (module_ != nullptr) return;
  std::wstring mode;
  std::wstring endpoint;
  if (!read_setting(L"RelayMode", &mode) || mode != L"application" ||
      !read_setting(L"RelayEndpointId", &endpoint) || endpoint.empty()) {
    return;
  }
  const auto directory = module_directory();
  if (directory.empty()) return;
  const auto path = directory + L"\\soundfork_app_relay.dll";
  HMODULE module = LoadLibraryExW(
      path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (module == nullptr) return;
  const auto start = reinterpret_cast<StartFunction>(
      GetProcAddress(module, "SoundForkAppRelayStart"));
  const auto stop = reinterpret_cast<StopFunction>(
      GetProcAddress(module, "SoundForkAppRelayStop"));
  if (start == nullptr || stop == nullptr ||
      start(endpoint.c_str()) != ERROR_SUCCESS) {
    FreeLibrary(module);
    return;
  }
  module_ = module;
  stop_ = stop;
}

void ApplicationRelay::stop() noexcept {
  if (module_ == nullptr) return;
  if (stop_ != nullptr) (void)stop_();
  FreeLibrary(module_);
  module_ = nullptr;
  stop_ = nullptr;
}

}  // namespace soundfork

