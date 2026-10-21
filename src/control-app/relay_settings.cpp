#include "control-app/relay_settings.hpp"

#include <algorithm>
#include <cwctype>
#include <utility>
#include <vector>

namespace soundfork::control {
namespace {
struct Key {
  HKEY value = nullptr;
  ~Key() { if (value != nullptr) RegCloseKey(value); }
};
bool same(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}
bool plain(std::wstring_view value) {
  return !value.empty() && value.find(L'\0') == value.npos &&
         value.find_first_of(L"\r\n") == value.npos;
}
DWORD read(HKEY root, const std::wstring& path, const wchar_t* name, std::wstring& text) {
  text.clear();
  Key key;
  auto code = RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &key.value);
  if (code != ERROR_SUCCESS) return code;
  DWORD bytes = 0, type = 0;
  code = RegQueryValueExW(key.value, name, nullptr, &type, nullptr, &bytes);
  if (code != ERROR_SUCCESS) return code;
  if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 8192 || bytes % sizeof(wchar_t) != 0)
    return ERROR_INVALID_DATA;
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
  code = RegQueryValueExW(key.value, name, nullptr, &type,
      reinterpret_cast<BYTE*>(buffer.data()), &bytes);
  if (code != ERROR_SUCCESS) return code;
  if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0 ||
      bytes / sizeof(wchar_t) > buffer.size()) return ERROR_INVALID_DATA;
  const auto length = bytes / sizeof(wchar_t);
  if (buffer[length - 1] != L'\0' ||
      std::find(buffer.begin(), buffer.begin() + length - 1, L'\0') != buffer.begin() + length - 1)
    return ERROR_INVALID_DATA;
  text.assign(buffer.data(), length - 1);
  return ERROR_SUCCESS;
}
DWORD write(HKEY root, const std::wstring& path, const wchar_t* name, std::wstring_view text) {
  if (root == nullptr) return ERROR_INVALID_HANDLE;
  Key key;
  auto code = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                            nullptr, &key.value, nullptr);
  if (code != ERROR_SUCCESS) return code;
  const std::wstring terminated(text);
  return RegSetValueExW(key.value, name, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(terminated.c_str()),
      static_cast<DWORD>((terminated.size() + 1) * sizeof(wchar_t)));
}
bool owned_command(std::wstring_view command) {
  constexpr std::wstring_view suffix = L"\" --startup";
  if (!command.starts_with(L'\"') || !command.ends_with(suffix) || command.size() <= suffix.size() + 1)
    return false;
  std::wstring generated;
  const auto path = command.substr(1, command.size() - suffix.size() - 1);
  return RelaySettings::make_startup_command(path, generated) == ERROR_SUCCESS && same(command, generated);
}
}  // namespace

RelaySettings::RelaySettings(HKEY root, std::wstring settings_path, std::wstring run_path)
    : root_(root), settings_path_(std::move(settings_path)), run_path_(std::move(run_path)) {}

std::wstring RelaySettings::endpoint() const {
  std::wstring value;
  if (read(root_, settings_path_, L"RelayEndpointId", value) != ERROR_SUCCESS ||
      !plain(value) || value.size() > 1024) return {};
  return value;
}
DWORD RelaySettings::save_endpoint(std::wstring_view id) const {
  if (!plain(id) || id.size() > 1024) return ERROR_INVALID_PARAMETER;
  return write(root_, settings_path_, L"RelayEndpointId", id);
}
DWORD RelaySettings::make_startup_command(std::wstring_view executable, std::wstring& command) {
  command.clear();
  const bool absolute = (executable.size() >= 3 && std::iswalpha(executable[0]) &&
      executable[1] == L':' && executable[2] == L'\\') || executable.starts_with(L"\\\\");
  const auto slash = executable.find_last_of(L"\\/");
  if (!absolute || !plain(executable) || executable.find(L'\"') != executable.npos ||
      slash == executable.npos || !same(executable.substr(slash + 1), L"soundfork_control.exe"))
    return ERROR_INVALID_PARAMETER;
  command = L"\"" + std::wstring(executable) + L"\" --startup";
  if (command.size() > 260) { command.clear(); return ERROR_FILENAME_EXCED_RANGE; }
  return ERROR_SUCCESS;
}
bool RelaySettings::startup_enabled(std::wstring_view executable) const {
  std::wstring expected, actual;
  return make_startup_command(executable, expected) == ERROR_SUCCESS &&
         read(root_, run_path_, L"SoundFork", actual) == ERROR_SUCCESS && same(actual, expected);
}
DWORD RelaySettings::set_startup(std::wstring_view executable, bool enabled) const {
  std::wstring command, previous;
  auto code = make_startup_command(executable, command);
  if (code != ERROR_SUCCESS) return code;
  code = read(root_, run_path_, L"SoundFork", previous);
  if (code != ERROR_SUCCESS && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) return code;
  if (code == ERROR_SUCCESS && !owned_command(previous)) return ERROR_ALREADY_EXISTS;
  if (enabled) {
    if (endpoint().empty()) return ERROR_NOT_FOUND;
    return write(root_, run_path_, L"SoundFork", command);
  }
  // Do not disable a different installed/build copy by accident.
  if (code == ERROR_SUCCESS && !same(previous, command)) return ERROR_ALREADY_EXISTS;
  return remove_startup(executable);
}
DWORD RelaySettings::remove_startup(std::wstring_view executable) const {
  std::wstring command, previous;
  auto code = make_startup_command(executable, command);
  if (code != ERROR_SUCCESS) return code;
  code = read(root_, run_path_, L"SoundFork", previous);
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_INVALID_DATA)
    return ERROR_SUCCESS;
  if (code != ERROR_SUCCESS || !same(previous, command)) return code;
  Key key;
  code = RegOpenKeyExW(root_, run_path_.c_str(), 0, KEY_SET_VALUE, &key.value);
  if (code != ERROR_SUCCESS) return code;
  code = RegDeleteValueW(key.value, L"SoundFork");
  return code == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : code;
}

ControlInstance::ControlInstance(const wchar_t* name) noexcept {
  handle_ = CreateMutexW(nullptr, FALSE, name);
  if (handle_ != nullptr) primary_ = GetLastError() != ERROR_ALREADY_EXISTS;
}
ControlInstance::~ControlInstance() { if (handle_ != nullptr) CloseHandle(handle_); }

}  // namespace soundfork::control
