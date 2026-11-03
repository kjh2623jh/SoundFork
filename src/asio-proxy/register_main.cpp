#include <windows.h>
#include <objbase.h>
#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include "audio-bridge/ids.hpp"

namespace {
struct Key { HKEY value = nullptr; ~Key() { if (value) RegCloseKey(value); } };
std::wstring read(HKEY key, const wchar_t* name) {
  std::array<wchar_t, 1024> text{};
  DWORD bytes = sizeof(text);
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, text.data(), &bytes) != ERROR_SUCCESS)
    return {};
  return text.data();
}
// Match the selected x64 vendor by CLSID, then by its ASIO registry name.
// Never choose an unrelated installed driver just because it is present.
std::wstring counterpart(std::wstring_view requested) {
  CLSID parsed{};
  const std::wstring clsid(requested);
  if (FAILED(CLSIDFromString(clsid.c_str(), &parsed)) ||
      IsEqualCLSID(parsed, soundfork::kSoundForkClsid)) return {};
  std::wstring selected_name;
  for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
    Key root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | view, &root.value) != ERROR_SUCCESS)
      continue;
    for (DWORD index = 0;; ++index) {
      std::array<wchar_t, 256> name{};
      DWORD length = static_cast<DWORD>(name.size());
      const auto result = RegEnumKeyExW(root.value, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
      if (result == ERROR_NO_MORE_ITEMS) break;
      if (result != ERROR_SUCCESS) continue;
      Key driver;
      if (RegOpenKeyExW(root.value, name.data(), 0, KEY_READ | view, &driver.value) != ERROR_SUCCESS) continue;
      const auto candidate = read(driver.value, L"CLSID");
      CLSID id{};
      if (FAILED(CLSIDFromString(candidate.c_str(), &id)) || IsEqualCLSID(id, soundfork::kSoundForkClsid)) continue;
      if (view == KEY_WOW64_64KEY) {
        if (IsEqualCLSID(id, parsed)) selected_name = name.data();
      } else if (IsEqualCLSID(id, parsed) || (!selected_name.empty() && _wcsicmp(selected_name.c_str(), name.data()) == 0)) {
        // Confirm COM registration in the same architecture view.
        Key com;
        const auto path = L"SOFTWARE\\Classes\\CLSID\\" + candidate + L"\\InprocServer32";
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &com.value) == ERROR_SUCCESS &&
            !read(com.value, nullptr).empty()) return candidate;
      }
    }
  }
  return {};
}
}

int wmain(int argc, wchar_t** argv) {
  const bool remove = argc == 2 && std::wstring_view(argv[1]) == L"--unregister";
  const bool install = argc == 3 && std::wstring_view(argv[1]) == L"--register";
  if (!remove && !install) return ERROR_INVALID_PARAMETER;
  static_assert(sizeof(void*) == 4);
  std::vector<wchar_t> path(32768);
  const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return ERROR_BAD_PATHNAME;
  std::wstring directory(path.data(), length);
  directory.resize(directory.find_last_of(L"\\/"));
  const auto dll_path = directory + L"\\soundfork_driver.dll";
  HMODULE dll = LoadLibraryExW(dll_path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) return static_cast<int>(GetLastError());
  using Entry = HRESULT(STDAPICALLTYPE*)();
  const auto entry = reinterpret_cast<Entry>(GetProcAddress(dll, remove ? "DllUnregisterServer" : "DllRegisterServer"));
  HRESULT result = entry ? entry() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  if (SUCCEEDED(result) && install) {
    const auto backend = counterpart(argv[2]);
    Key key;
    auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, soundfork::kDriverRegistryPath, 0,
        KEY_SET_VALUE | KEY_WOW64_32KEY, &key.value);
    if (status == ERROR_SUCCESS) {
      if (backend.empty()) {
        status = RegDeleteValueW(key.value, L"BackendCLSID");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
        std::wcout << L"No matching x86 ASIO backend. Install the vendor's 32-bit ASIO driver.\n";
      } else {
        status = RegSetValueExW(key.value, L"BackendCLSID", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(backend.c_str()), static_cast<DWORD>((backend.size() + 1) * sizeof(wchar_t)));
      }
    }
    result = HRESULT_FROM_WIN32(status);
  }
  FreeLibrary(dll);
  return FAILED(result) ? static_cast<int>(result) : 0;
}
