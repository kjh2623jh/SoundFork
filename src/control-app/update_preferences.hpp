#pragma once
#include <windows.h>

namespace soundfork::control {
inline bool automatic_updates_enabled(HKEY root = HKEY_CURRENT_USER,
    const wchar_t* path = L"Software\\SoundFork") noexcept {
  DWORD value = 1, bytes = sizeof(value);
  return RegGetValueW(root, path, L"AutomaticUpdateNotifications", RRF_RT_REG_DWORD,
      nullptr, &value, &bytes) != ERROR_SUCCESS || value != 0;
}
inline DWORD save_automatic_updates(bool enabled, HKEY root = HKEY_CURRENT_USER,
    const wchar_t* path = L"Software\\SoundFork") noexcept {
  HKEY key = nullptr;
  auto result = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (result == ERROR_SUCCESS) {
    const DWORD value = enabled ? 1U : 0U;
    result = RegSetValueExW(key, L"AutomaticUpdateNotifications", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
  }
  return result;
}
} // namespace soundfork::control
