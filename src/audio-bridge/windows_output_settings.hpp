#pragma once
#include <windows.h>
#include <array>
#include <string>
namespace soundfork {
inline HRESULT read_windows_output(std::wstring* endpoint) {
  endpoint->clear();
  HKEY key = nullptr;
  auto result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SoundFork\\Output", 0,
      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
  if(result == ERROR_FILE_NOT_FOUND) return S_FALSE;
  if(result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);
  std::array<wchar_t, 1025> text{}; DWORD bytes = sizeof(text);
  result = RegGetValueW(key, nullptr, L"WindowsEndpoint", RRF_RT_REG_SZ, nullptr, text.data(), &bytes);
  RegCloseKey(key);
  if(result == ERROR_FILE_NOT_FOUND) return S_FALSE;
  if(result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);
  if(text[0] == L'\0') return S_FALSE;
  *endpoint = text.data(); return S_OK;
}
inline DWORD save_windows_output(const std::wstring& endpoint) noexcept {
  if(endpoint.size() > 1024 || endpoint.find(L'\0') != std::wstring::npos) return ERROR_INVALID_DATA;
  HKEY key = nullptr;
  auto result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SoundFork\\Output", 0,
      nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
  if(result != ERROR_SUCCESS) return result;
  if(endpoint.empty()) {
    result = RegDeleteValueW(key, L"WindowsEndpoint");
    if(result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
  } else result = RegSetValueExW(key, L"WindowsEndpoint", 0, REG_SZ,
      reinterpret_cast<const BYTE*>(endpoint.c_str()), static_cast<DWORD>((endpoint.size()+1)*sizeof(wchar_t)));
  RegCloseKey(key); return result;
}
} // namespace soundfork
