#pragma once

#include <windows.h>

namespace soundfork {

// Optional process-local WASAPI relay. It is activated only by an explicit
// user setting and only from ASIO lifecycle methods, never from a callback.
class ApplicationRelay final {
 public:
  ApplicationRelay() = default;
  ~ApplicationRelay();
  ApplicationRelay(const ApplicationRelay&) = delete;
  ApplicationRelay& operator=(const ApplicationRelay&) = delete;

  void start_if_enabled() noexcept;
  void stop() noexcept;
  bool loaded() const noexcept { return module_ != nullptr; }

 private:
  using StartFunction = DWORD(WINAPI*)(const wchar_t*);
  using StopFunction = DWORD(WINAPI*)();
  HMODULE module_ = nullptr;
  StopFunction stop_ = nullptr;
};

}  // namespace soundfork

