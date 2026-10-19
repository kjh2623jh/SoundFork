#pragma once

#include <windows.h>

#include <string>

namespace soundfork::capture {

enum class HubRole { relay, obs };

// Each output retains the existing bounded SPSC protocol. All fan-out runs in
// the hub process, never in the ASIO callback. A scope isolates automated tests.
struct HubNames {
  explicit HubNames(const std::wstring& scope = L"");
  std::wstring input;
  std::wstring relay;
  std::wstring obs;
  std::wstring singleton;
  std::wstring ready;
  std::wstring relay_lease;
  std::wstring obs_lease;
};

std::wstring current_module_directory();
HANDLE create_periodic_timer(DWORD milliseconds) noexcept;
bool set_timer_period(HANDLE timer, DWORD milliseconds) noexcept;
bool hub_has_clients(const HubNames& names) noexcept;

// Hold a lease even while waiting for the game. Windows closes it on a crash.
// Construct/ensure only from control or capture-worker threads.
class HubClient final {
 public:
  explicit HubClient(HubRole role, const std::wstring& scope = L"");
  ~HubClient();
  HubClient(const HubClient&) = delete;
  HubClient& operator=(const HubClient&) = delete;
  bool ensure_running();
  const wchar_t* mapping_name() const noexcept;

 private:
  HubNames names_;
  HubRole role_;
  HANDLE lease_ = nullptr;
  ULONGLONG next_launch_ = 0;
};

}  // namespace soundfork::capture
