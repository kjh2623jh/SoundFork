#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace soundfork::capture {
// Read only during stream setup, never from the ASIO callback.
inline DWORD relay_preference(const wchar_t* name, DWORD fallback) noexcept {
  DWORD value = 0, bytes = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, L"Software\\SoundFork", name,
      RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS ? value : fallback;
}
inline bool relay_enabled() noexcept {
  return relay_preference(L"RelayEnabled", 1) != 0;
}
inline DWORD save_relay_preference(const wchar_t* name, DWORD value) noexcept {
  HKEY key = nullptr;
  auto result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SoundFork", 0,
      nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (result == ERROR_SUCCESS) {
    result = RegSetValueExW(key, name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
  }
  return result;
}
inline std::uint32_t relay_target_frames(std::uint32_t block, DWORD requested) noexcept {
  if (block == 0 || requested == 0 || requested > 9600) return block * 4;
  // Leave room for scheduling jitter and interpolation in the 64-block ring.
  return (std::clamp)(static_cast<std::uint32_t>(requested), block, block * 32);
}
}  // namespace soundfork::capture
