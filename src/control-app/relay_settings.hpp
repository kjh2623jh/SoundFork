#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace soundfork::control {

// UI/startup only. Tests supply a private registry root: no environment override
// can redirect production startup or installer operations.
class RelaySettings final {
 public:
  explicit RelaySettings(HKEY root = HKEY_CURRENT_USER,
      std::wstring settings_path = L"Software\\SoundFork",
      std::wstring run_path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run");
  std::wstring endpoint() const;
  DWORD save_endpoint(std::wstring_view id) const;
  bool startup_enabled(std::wstring_view executable) const;
  DWORD set_startup(std::wstring_view executable, bool enabled) const;
  // Uninstall only removes an exact registration for this installed executable.
  DWORD remove_startup(std::wstring_view executable) const;

  static DWORD make_startup_command(std::wstring_view executable, std::wstring& command);

 private:
  HKEY root_;
  std::wstring settings_path_;
  std::wstring run_path_;
};

// Hold for the GUI lifetime, not for installer/probe command invocations.
class ControlInstance final {
 public:
  explicit ControlInstance(const wchar_t* name) noexcept;
  ~ControlInstance();
  ControlInstance(const ControlInstance&) = delete;
  ControlInstance& operator=(const ControlInstance&) = delete;
  bool valid() const noexcept { return handle_ != nullptr; }
  bool primary() const noexcept { return primary_; }
 private:
  HANDLE handle_ = nullptr;
  bool primary_ = false;
};

}  // namespace soundfork::control
