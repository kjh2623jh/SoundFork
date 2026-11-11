#pragma once

#include <windows.h>
#include <utility>

namespace soundfork::platform {
// Owns only handles closed by CloseHandle (not HKEY, HWND or mapped views).
// Both documented failure sentinels are accepted. No allocation or locking.
class UniqueHandle final {
 public:
  explicit UniqueHandle(HANDLE handle = nullptr) noexcept : value(handle) {}
  ~UniqueHandle() { reset(); }
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
  UniqueHandle(UniqueHandle&& other) noexcept : value(other.release()) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  explicit operator bool() const noexcept {
    return value != nullptr && value != INVALID_HANDLE_VALUE;
  }
  HANDLE release() noexcept { return std::exchange(value, nullptr); }
  void reset(HANDLE replacement = nullptr) noexcept {
    if (value == replacement) return;
    if (*this) CloseHandle(value);
    value = replacement;
  }
  // Existing initialization sites write this once; use reset() for replacement.
  HANDLE value;
};
}  // namespace soundfork::platform
