// Fault injection uses process-private preferences and a loader that never
// loads a DLL. No actual relay, hub, endpoint or installed setting is used.
#include <windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#include <cstring>
#include <process.h>
namespace {
int __cdecl fail_debug_report(int type, char* message, int*) {
  if (type != _CRT_ASSERT && type != _CRT_ERROR) return FALSE;
  if (message) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message,
              static_cast<DWORD>(std::strlen(message)), &written, nullptr);
  }
  _exit(3); // A CRT assertion is a failed test, never a modal CI dialog.
}
struct DebugReports {
  DebugReports() { _CrtSetReportHook(fail_debug_report); }
} debug_reports;
}  // namespace
#endif
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

namespace {
std::atomic<int> fail_after{-1};
std::atomic<unsigned> injected{0};
std::atomic<unsigned> load_attempts{0};
HMODULE WINAPI deny_load(LPCWSTR, HANDLE, DWORD) {
  ++load_attempts;
  SetLastError(ERROR_MOD_NOT_FOUND);
  return nullptr;
}
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct PrivatePreferences {
  HKEY key = nullptr;
  std::wstring path = L"Software\\SoundForkAllocationTest-" + std::to_wstring(GetCurrentProcessId());
  PrivatePreferences() {
    require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS, "create private preferences");
    if (RegOverridePredefKey(HKEY_CURRENT_USER, key) != ERROR_SUCCESS) {
      RegCloseKey(key); key = nullptr;
      RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
      throw std::runtime_error("override private preferences");
    }
  }
  ~PrivatePreferences() {
    fail_after.store(-1);
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (key) RegCloseKey(key);
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  }
};
}  // namespace

_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(bytes) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t bytes) {
  bool inject_here = true;
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
  // MSVC allocates iterator bookkeeping in noexcept container constructors.
  // Failing that bookkeeping terminates inside the STL, before our boundary
  // can catch anything. Keep iterator checking enabled, and inject into the
  // application's larger string/vector payload allocations instead.
  inject_here = bytes != sizeof(std::_Container_proxy);
#endif
  if (inject_here && fail_after.load() >= 0 && fail_after.fetch_sub(1) == 0) {
    ++injected;
    throw std::bad_alloc();
  }
  if (void* result = std::malloc(bytes ? bytes : 1)) return result;
  throw std::bad_alloc();
}
_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(bytes) _VCRT_ALLOCATOR
void* __CRTDECL operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

#define LoadLibraryExW deny_load
#include "../src/audio-bridge/application_relay.cpp"
#undef LoadLibraryExW

int main() {
  try {
    PrivatePreferences preferences;
    soundfork::platform::UniqueRegistryKey settings;
    require(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SoundFork", 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &settings.value, nullptr) == ERROR_SUCCESS, "create settings");
    const DWORD enabled = 1;
    const wchar_t mode[] = L"application";
    const wchar_t endpoint[] = L"isolated-allocation-test-endpoint";
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
    static_assert(sizeof(mode) > sizeof(std::_Container_proxy));
    static_assert(sizeof(endpoint) > sizeof(std::_Container_proxy));
#endif
    require(RegSetValueExW(settings.value,L"RelayEnabled",0,REG_DWORD,
        reinterpret_cast<const BYTE*>(&enabled),sizeof(enabled)) == ERROR_SUCCESS, "write enabled");
    require(RegSetValueExW(settings.value,L"RelayMode",0,REG_SZ,
        reinterpret_cast<const BYTE*>(mode),sizeof(mode)) == ERROR_SUCCESS, "write mode");
    require(RegSetValueExW(settings.value,L"RelayEndpointId",0,REG_SZ,
        reinterpret_cast<const BYTE*>(endpoint),sizeof(endpoint)) == ERROR_SUCCESS, "write endpoint");
    soundfork::ApplicationRelay relay;
    relay.start_if_enabled(); // Warm up OS/CRT state; loader still denies DLLs.
    DWORD before = 0, after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(),&before) != FALSE, "initial handles");
    for (int index = 0; index < 16; ++index) {
      fail_after.store(index);
      relay.start_if_enabled();
      fail_after.store(-1);
      require(!relay.loaded(), "a real relay was loaded");
    }
    require(GetProcessHandleCount(GetCurrentProcess(),&after) != FALSE, "final handles");
    require(injected.load() >= 3, "allocation failure paths were not exercised");
    require(load_attempts.load() > 0, "normal setup did not reach the fake loader");
    require(after <= before, "allocation failures leaked registry handles");
    std::cout << "Optional relay allocation failures isolated: injected=" << injected.load()
              << " handles_before=" << before << " handles_after=" << after << '\n';
    return 0;
  } catch (const std::exception& error) {
    fail_after.store(-1);
    std::cerr << error.what() << '\n'; return 1;
  }
}
