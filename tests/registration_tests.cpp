// Redirect HKLM for this process only. Never register into the installed system.
#define wmain registration_entry
#include "../src/asio-proxy/register_main.cpp"
#undef wmain
#include <stdexcept>

namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void put(const wchar_t* path, const wchar_t* name, const wchar_t* value) {
  Key key;
  check(RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
      &key.value, nullptr) == ERROR_SUCCESS, "fixture key");
  check(RegSetValueExW(key.value, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
      static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS, "fixture value");
}
}
int wmain() {
  const auto root_path = L"Software\\SoundForkRegistrationTest-" + std::to_wstring(GetCurrentProcessId());
  Key root;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, root_path.c_str(), 0, nullptr, 0,
      KEY_ALL_ACCESS, nullptr, &root.value, nullptr) != ERROR_SUCCESS) return 2;
  if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, root.value) != ERROR_SUCCESS) return 3;
  int result = 0;
  try {
    wchar_t exe[] = L"register-test";
    wchar_t add[] = L"--register";
    wchar_t remove[] = L"--unregister";
    wchar_t vendor[] = L"{7C8656E5-9B2D-44E9-A43A-D767CF8B92F2}";
    put(L"SOFTWARE\\ASIO\\TestVendor", L"CLSID", vendor);
    const auto com = L"SOFTWARE\\Classes\\CLSID\\" + std::wstring(vendor) + L"\\InprocServer32";
    put(com.c_str(), nullptr, L"test-vendor.dll");
    wchar_t* args[] = {exe, add, vendor};
    check(registration_entry(3, args) == 0, "registration failed");
    check(registration_entry(3, args) == 0, "reinstall failed");
    {
      Key driver;
      check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, soundfork::kDriverRegistryPath, 0, KEY_READ,
          &driver.value) == ERROR_SUCCESS && read(driver.value, L"BackendCLSID") == vendor, "backend mismatch");
    }
    // Removing the vendor's COM registration must clear the stale backend only.
    check(RegDeleteTreeW(HKEY_LOCAL_MACHINE, com.c_str()) == ERROR_SUCCESS, "remove fixture COM");
    check(registration_entry(3, args) == 0, "no-counterpart registration failed");
    {
      Key driver;
      check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, soundfork::kDriverRegistryPath, 0, KEY_READ,
          &driver.value) == ERROR_SUCCESS && read(driver.value, L"BackendCLSID").empty(), "stale x86 backend retained");
    }
    wchar_t* unreg[] = {exe, remove};
    check(registration_entry(2, unreg) == 0 && registration_entry(2, unreg) == 0, "unregister/repeat failed");
    Key driver;
    check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, soundfork::kDriverRegistryPath, 0, KEY_READ,
        &driver.value) == ERROR_FILE_NOT_FOUND, "proxy remains registered");
    check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\TestVendor", 0, KEY_READ,
        &driver.value) == ERROR_SUCCESS, "vendor registration removed");
    std::cout << "Isolated x86 registration/install/reinstall/missing-backend/unregister passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
  RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
  RegCloseKey(root.value); root.value = nullptr;
  RegDeleteTreeW(HKEY_CURRENT_USER, root_path.c_str());
  return result;
}
