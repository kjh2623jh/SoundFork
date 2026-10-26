#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <exception>

#include "control-app/relay_settings.hpp"

namespace {
using soundfork::control::RelaySettings;
using soundfork::control::ControlInstance;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Phase {
  const char* name;
  ULONGLONG started = GetTickCount64();
  int exceptions = std::uncaught_exceptions();
  explicit Phase(const char* value) : name(value) {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char stamp[40]{};
    _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    std::cout << stamp << " START " << name << std::endl;
  }
  ~Phase() {
    std::cout << (std::uncaught_exceptions() > exceptions ? "FAIL " : "END ") << name
              << " elapsed_ms=" << GetTickCount64() - started << std::endl;
  }
};

struct OwnedChild {
  PROCESS_INFORMATION info{};
  bool cleanup_attempted = false;
  bool terminate() noexcept {
    cleanup_attempted = true;
    const BOOL requested = TerminateProcess(info.hProcess, 255);
    const auto result = WaitForSingleObject(info.hProcess, 2000);
    if (!requested && result != WAIT_OBJECT_0) return false;
    return result == WAIT_OBJECT_0;
  }
  ~OwnedChild() {
    if (info.hProcess != nullptr) {
      if (!cleanup_attempted && WaitForSingleObject(info.hProcess, 0) != WAIT_OBJECT_0 && !terminate())
        std::cerr << "Child cleanup failed pid=" << info.dwProcessId << std::endl;
      CloseHandle(info.hProcess);
    }
    if (info.hThread != nullptr) CloseHandle(info.hThread);
  }
};
struct ChildResult { DWORD code; bool timed_out; };
ChildResult run_child(const wchar_t* executable, std::wstring command, const char* stage,
                      DWORD timeout_ms = 10000) {
  Phase phase(stage);
  STARTUPINFOW startup{sizeof(startup)};
  OwnedChild child;
  std::cout << "Creating test child timeout_ms=" << timeout_ms << std::endl;
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &child.info)) {
    const auto error = GetLastError();
    throw std::runtime_error(std::string(stage) + " CreateProcess win32=" + std::to_string(error));
  }
  // No thread operations are needed after launch. Release this handle immediately
  // rather than retaining it through a potentially long process wait.
  CloseHandle(child.info.hThread);
  child.info.hThread = nullptr;
  std::cout << "Waiting for test child pid=" << child.info.dwProcessId << std::endl;
  const auto waited = WaitForSingleObject(child.info.hProcess, timeout_ms);
  const auto wait_error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
  bool cleaned = true;
  if (waited != WAIT_OBJECT_0) {
    // Only the exact child created by this fixture, never image-name termination.
    cleaned = child.terminate();
  }
  DWORD code = 255;
  const BOOL queried = GetExitCodeProcess(child.info.hProcess, &code);
  std::cout << "Child result stage=" << stage << " pid=" << child.info.dwProcessId
            << " wait=" << waited << " win32=" << wait_error << " exit_code=" << code
            << " cleanup_complete=" << cleaned << std::endl;
  require(cleaned && queried, "test child exit/cleanup failed");
  require(waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT, "test child wait failed");
  return {code, waited == WAIT_TIMEOUT};
}

std::wstring test_executable() {
  std::vector<wchar_t> exe(32768);
  const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
  require(length != 0 && length < exe.size(), "exe path failed");
  return {exe.data(), length};
}
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
  const auto exe = test_executable();
  const auto result = run_child(exe.c_str(), L"\"" + exe + L"\" --instance-probe " + name, "instance-child");
  require(!result.timed_out, "instance probe timed out after cleanup");
  return result.code;
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
  std::cout << "Copying private renamed control fixture" << std::endl;
  require(CopyFileW(control, copy.c_str(), TRUE) != FALSE, "control fixture copy failed");
  struct Remove { const std::wstring& path; ~Remove() { DeleteFileW(path.c_str()); } } remove{copy};
  const auto result = run_child(copy.c_str(), L"\"" + copy + L"\" --startup", "disabled-startup-child");
  require(!result.timed_out && result.code == 0, "disabled startup did not exit quietly after cleanup");
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
void test_timeout_cleanup() {
  const auto exe = test_executable();
  const auto result = run_child(exe.c_str(), L"\"" + exe + L"\" --slow-child", "timeout-cleanup", 100);
  require(result.timed_out && result.code == 255, "intentional timeout did not terminate/reap its child");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"--slow-child") {
    Sleep(60000);  // Isolated fixture: parent must terminate it at its deadline.
    return 0;
  }
  if (argc == 3 && std::wstring_view(argv[1]) == L"--instance-probe") {
    ControlInstance instance(argv[2]);
    return !instance.valid() ? 3 : (instance.primary() ? 0 : 2);
  }
  try {
    const bool selected = argc >= 3 && std::wstring_view(argv[1]) == L"--case";
    const std::wstring_view name = selected ? argv[2] : L"all";
    require((!selected && argc <= 2) || (selected &&
        ((argc == 3 && (name == L"settings" || name == L"instances")) ||
         (argc == 4 && name == L"disabled-startup"))), "invalid test case arguments");
    if (name == L"settings" || name == L"all") {
      { Phase phase("settings"); test_settings(); }
      { Phase phase("access-denied"); test_access_denied(); }
    }
    if (name == L"instances" || name == L"all") {
      { Phase phase("instances"); test_instances(); }
      { Phase phase("child-timeout-cleanup"); test_timeout_cleanup(); }
    }
    if (name == L"disabled-startup" || (!selected && argc == 2)) {
      Phase phase("disabled-startup");
      test_disabled_startup_command(selected ? argv[3] : argv[1]);
    }
    std::cout << "Selected relay settings/startup tests passed" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
