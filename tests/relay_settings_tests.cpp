#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "control-app/relay_settings.hpp"

namespace {
using soundfork::control::RelaySettings;
using soundfork::control::ControlInstance;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct RegistryFixture {
  HKEY root = nullptr;
  std::wstring path;
  RegistryFixture() {
    GUID guid{};
    require(SUCCEEDED(CoCreateGuid(&guid)), "GUID failed");
    std::array<wchar_t, 40> text{};
    require(StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) != 0, "GUID text failed");
    path = L"Software\\SoundForkSettingsTest-" + std::wstring(text.data());
    require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS, "fixture registry failed");
  }
  ~RegistryFixture() {
    if (root != nullptr) RegCloseKey(root);
    // Exact fresh test key, never the user's SoundFork or Windows Run key.
    if (path.starts_with(L"Software\\SoundForkSettingsTest-")) RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  }
};
void put(HKEY root, const wchar_t* name, const std::wstring& value, DWORD type = REG_SZ) {
  HKEY key = nullptr;
  require(RegCreateKeyExW(root, L"Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
          == ERROR_SUCCESS, "fixture Run failed");
  const auto code = RegSetValueExW(key, name, 0, type,
      reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  require(code == ERROR_SUCCESS, "fixture value failed");
}
void test_settings() {
  RegistryFixture fixture;
  RelaySettings settings(fixture.root, L"Settings", L"Run");
  const std::wstring exe = L"C:\\Program Files\\사운드 포크\\soundfork_control.exe";
  require(settings.endpoint().empty(), "new endpoint is not empty");
  require(!settings.startup_enabled(exe), "startup enabled by default");
  require(settings.set_startup(exe, true) == ERROR_NOT_FOUND, "startup accepted missing endpoint");
  require(settings.save_endpoint(L"{0.0.0.00000000}.{fixture}") == ERROR_SUCCESS, "save failed");
  RelaySettings reopened(fixture.root, L"Settings", L"Run");
  require(reopened.endpoint() == L"{0.0.0.00000000}.{fixture}", "endpoint did not persist");
  std::wstring command;
  require(RelaySettings::make_startup_command(exe, command) == ERROR_SUCCESS &&
          command == L"\"" + exe + L"\" --startup", "path not quoted");
  require(settings.set_startup(exe, true) == ERROR_SUCCESS, "enable failed");
  require(reopened.startup_enabled(exe), "enable did not persist");
  require(settings.remove_startup(L"D:\\Another\\soundfork_control.exe") == ERROR_SUCCESS,
          "unrelated uninstall failed");
  require(settings.startup_enabled(exe), "uninstall removed another copy's entry");
  put(fixture.root, L"UnrelatedApp", L"leave this alone");
  require(settings.set_startup(exe, false) == ERROR_SUCCESS, "disable failed");
  require(!settings.startup_enabled(exe), "disable did not persist");
  require(settings.endpoint() == L"{0.0.0.00000000}.{fixture}", "disable erased endpoint");
  require(settings.set_startup(exe, true) == ERROR_SUCCESS, "re-enable failed");
  // A new copy may replace an explicitly recognized SoundFork registration.
  const std::wstring next = L"D:\\New copy\\soundfork_control.exe";
  require(settings.set_startup(next, true) == ERROR_SUCCESS && settings.startup_enabled(next), "copy migration failed");
  require(settings.remove_startup(next) == ERROR_SUCCESS && !settings.startup_enabled(next), "cleanup failed");
  HKEY key = nullptr;
  require(RegOpenKeyExW(fixture.root, L"Run", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS, "Run disappeared");
  DWORD bytes = 0;
  const auto code = RegQueryValueExW(key, L"UnrelatedApp", nullptr, nullptr, nullptr, &bytes);
  RegCloseKey(key);
  require(code == ERROR_SUCCESS, "cleanup removed unrelated startup entry");
  put(fixture.root, L"SoundFork", L"\"C:\\other.exe\" --startup");
  require(settings.set_startup(exe, true) == ERROR_ALREADY_EXISTS, "foreign entry overwritten");
  require(settings.remove_startup(exe) == ERROR_SUCCESS, "foreign cleanup failed");
  require(settings.set_startup(exe, true) == ERROR_ALREADY_EXISTS, "foreign entry removed");
  put(fixture.root, L"SoundFork", command, REG_EXPAND_SZ);
  require(settings.set_startup(exe, true) == ERROR_INVALID_DATA, "wrong type overwritten");
  require(!settings.startup_enabled(exe), "wrong registry type enabled startup");
  put(fixture.root, L"SoundFork", std::wstring(L"bad\0embedded", 12));
  require(settings.set_startup(exe, true) == ERROR_INVALID_DATA, "embedded null accepted");
  require(settings.save_endpoint(std::wstring_view(L"a\0b", 3)) == ERROR_INVALID_PARAMETER, "endpoint null accepted");
  require(settings.save_endpoint(L"") == ERROR_INVALID_PARAMETER, "empty endpoint accepted");
  require(RelaySettings::make_startup_command(L"soundfork_control.exe", command) == ERROR_INVALID_PARAMETER, "relative path accepted");
  require(RelaySettings::make_startup_command(L"C:\\bad\"\\soundfork_control.exe", command) == ERROR_INVALID_PARAMETER, "quote injection accepted");
  require(RelaySettings::make_startup_command(L"C:\\" + std::wstring(260, L'x') + L"\\soundfork_control.exe", command)
          == ERROR_FILENAME_EXCED_RANGE, "overlong Run command accepted");
  const auto failed = RelaySettings(nullptr, L"", L"").save_endpoint(L"different");
  require(failed == ERROR_INVALID_HANDLE, "write failure was hidden");
  require(settings.endpoint() == L"{0.0.0.00000000}.{fixture}", "failed save changed the endpoint");
}

void test_access_denied() {
  RegistryFixture fixture;
  HKEY denied = nullptr;
  require(RegCreateKeyExW(fixture.root, L"Denied", 0, nullptr, 0, KEY_ALL_ACCESS,
      nullptr, &denied, nullptr) == ERROR_SUCCESS, "denied fixture failed");
  DWORD bytes = 0;
  require(RegGetKeySecurity(denied, DACL_SECURITY_INFORMATION, nullptr, &bytes)
          == ERROR_INSUFFICIENT_BUFFER && bytes != 0, "DACL size failed");
  std::vector<BYTE> original(bytes);
  require(RegGetKeySecurity(denied, DACL_SECURITY_INFORMATION,
      reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data()), &bytes) == ERROR_SUCCESS, "DACL backup failed");
  struct Restore {
    HKEY key;
    PSECURITY_DESCRIPTOR security;
    ~Restore() { RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, security); RegCloseKey(key); }
  } restore{denied, reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data())};
  PSECURITY_DESCRIPTOR restriction = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:(D;;0x2;;;WD)(A;;KA;;;WD)", SDDL_REVISION_1, &restriction, nullptr) != FALSE, "test ACL failed");
  const auto changed = RegSetKeySecurity(denied, DACL_SECURITY_INFORMATION, restriction);
  LocalFree(restriction);
  require(changed == ERROR_SUCCESS, "test ACL application failed");
  RelaySettings settings(fixture.root, L"Settings", L"Denied");
  require(settings.save_endpoint(L"test-virtual-endpoint") == ERROR_SUCCESS, "test endpoint failed");
  require(settings.set_startup(L"C:\\test\\soundfork_control.exe", true) == ERROR_ACCESS_DENIED,
          "startup registration access failure was hidden");
  require(!settings.startup_enabled(L"C:\\test\\soundfork_control.exe"), "failed registration enabled startup");
}

DWORD child_probe(const std::wstring& name) {
  std::vector<wchar_t> exe(32768);
  require(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())) != 0, "exe path failed");
  std::wstring command = L"\"" + std::wstring(exe.data()) + L"\" --instance-probe " + name;
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  require(CreateProcessW(exe.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process) != FALSE, "probe process failed");
  const auto waited = WaitForSingleObject(process.hProcess, 5000);
  DWORD code = 255;
  const auto exited = GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  require(waited == WAIT_OBJECT_0 && exited, "probe did not finish");
  return code;
}

void test_disabled_startup_command(const wchar_t* control) {
  // The deliberately different executable basename cannot match any valid Run
  // registration. Exercise the real parser without changing user settings or
  // starting a relay/GUI, even when installed SoundFork is already running.
  std::vector<wchar_t> temp(32768);
  const auto temp_length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
  require(temp_length != 0 && temp_length < temp.size(), "temp path failed");
  GUID guid{};
  require(SUCCEEDED(CoCreateGuid(&guid)), "temp GUID failed");
  std::array<wchar_t, 40> guid_text{};
  require(StringFromGUID2(guid, guid_text.data(), static_cast<int>(guid_text.size())) != 0,
          "temp GUID text failed");
  const auto copy = std::wstring(temp.data()) + L"SoundFork-startup-test-" + guid_text.data() + L".exe";
  require(CopyFileW(control, copy.c_str(), TRUE) != FALSE, "control fixture copy failed");
  struct Remove { const std::wstring& path; ~Remove() { DeleteFileW(path.c_str()); } } remove{copy};
  std::wstring command = L"\"" + copy + L"\" --startup";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  require(CreateProcessW(copy.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process) != FALSE, "disabled startup process failed");
  const auto waited = WaitForSingleObject(process.hProcess, 5000);
  DWORD code = 255;
  GetExitCodeProcess(process.hProcess, &code);
  if (waited != WAIT_OBJECT_0) {
    // Only the test's exact, freshly created child; never a user's process.
    TerminateProcess(process.hProcess, 255);
    WaitForSingleObject(process.hProcess, 5000);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  require(waited == WAIT_OBJECT_0 && code == 0, "disabled startup did not exit quietly");
}
void test_instances() {
  const auto name = L"Local\\SoundFork.Control.Test-" + std::to_wstring(GetCurrentProcessId());
  {
    ControlInstance first(name.c_str());
    require(first.valid() && first.primary(), "first instance failed");
    require(child_probe(name) == 2, "second process became primary");
  }
  require(child_probe(name) == 0, "instance was not released after exit");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring_view(argv[1]) == L"--instance-probe") {
    ControlInstance instance(argv[2]);
    return !instance.valid() ? 3 : (instance.primary() ? 0 : 2);
  }
  try {
    test_settings();
    test_access_denied();
    test_instances();
    if (argc == 2) test_disabled_startup_command(argv[1]);
    std::cout << "Relay settings/startup ownership, error handling and cross-process instance tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
