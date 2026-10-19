#include "capture-backend/hub_client.hpp"

#include <array>
#include <vector>

#include "capture-backend/shared_ring.hpp"

namespace soundfork::capture {
namespace {
bool event_exists(const std::wstring& name) noexcept {
  HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
  if (event == nullptr) {
    return false;
  }
  CloseHandle(event);
  return true;
}
}  // namespace

HubNames::HubNames(const std::wstring& scope) {
  const auto suffix = scope.empty() ? L"" : L"." + scope;
  input = std::wstring(kDefaultMappingName) + suffix;
  const auto root = L"Local\\SoundFork.CaptureHub.v1" + suffix;
  relay = root + L".Relay";
  obs = root + L".OBS";
  singleton = root + L".Singleton";
  ready = root + L".Ready";
  relay_lease = root + L".RelayClient";
  obs_lease = root + L".OBSClient";
}

std::wstring current_module_directory() {
  static const wchar_t anchor = 0;
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          &anchor, &module)) {
    return {};
  }
  std::vector<wchar_t> path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    return {};
  }
  const std::wstring full(path.data(), length);
  const auto separator = full.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"" : full.substr(0, separator);
}

HANDLE create_periodic_timer(DWORD milliseconds) noexcept {
  HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
      TIMER_MODIFY_STATE | SYNCHRONIZE);
  if (timer == nullptr) {
    timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  }
  if (timer == nullptr) {
    return nullptr;
  }
  if (!set_timer_period(timer, milliseconds)) {
    CloseHandle(timer);
    return nullptr;
  }
  return timer;
}

bool set_timer_period(HANDLE timer, DWORD milliseconds) noexcept {
  if (timer == nullptr || milliseconds == 0 || milliseconds > 1000) return false;
  LARGE_INTEGER due{};
  due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000;
  return SetWaitableTimer(timer, &due, static_cast<LONG>(milliseconds),
                         nullptr, nullptr, FALSE) != FALSE;
}

bool hub_has_clients(const HubNames& names) noexcept {
  return event_exists(names.relay_lease) || event_exists(names.obs_lease);
}

HubClient::HubClient(HubRole role, const std::wstring& scope)
    : names_(scope), role_(role) {
  if (scope.empty()) {
    std::array<wchar_t, 65> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"SOUNDFORK_CAPTURE_SCOPE", value.data(), static_cast<DWORD>(value.size()));
    if (length > 0 && length < value.size()) {
      const std::wstring requested(value.data(), length);
      if (requested.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") ==
          std::wstring::npos) {
        names_ = HubNames(requested);
      }
    }
  }
  const auto& name = role == HubRole::relay ? names_.relay_lease
                                           : names_.obs_lease;
  lease_ = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
}

HubClient::~HubClient() {
  if (lease_ != nullptr) {
    CloseHandle(lease_);
  }
}

bool HubClient::ensure_running() {
  if (lease_ == nullptr) {
    return false;
  }
  HANDLE ready = OpenEventW(SYNCHRONIZE, FALSE, names_.ready.c_str());
  if (ready != nullptr) {
    const bool running = WaitForSingleObject(ready, 0) == WAIT_OBJECT_0;
    CloseHandle(ready);
    if (running) {
      return true;
    }
  }
  if (GetTickCount64() < next_launch_) {
    return false;
  }
  next_launch_ = GetTickCount64() + 1000;
  const auto directory = current_module_directory();
  if (directory.empty()) {
    return false;
  }
  const auto path = directory + L"\\soundfork_capture_hub.exe";
  const std::wstring suffix = names_.input.substr(
      std::wstring(kDefaultMappingName).size());
  std::wstring command = L"\"" + path + L"\"";
  if (!suffix.empty()) {
    command += L" --scope " + suffix.substr(1);
  }
  std::vector<wchar_t> writable(command.begin(), command.end());
  writable.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(path.c_str(), writable.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, directory.c_str(),
                      &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return false;  // readiness is checked on the next worker iteration
}

const wchar_t* HubClient::mapping_name() const noexcept {
  return role_ == HubRole::relay ? names_.relay.c_str() : names_.obs.c_str();
}
}  // namespace soundfork::capture
