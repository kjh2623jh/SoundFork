#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "capture-backend/relay_control.hpp"
#include "capture-backend/relay_entry.hpp"
#include "diagnostics/async_log.hpp"

namespace {

std::thread g_worker;
std::atomic<bool> g_running{false};

DWORD stop_worker() noexcept {
  HANDLE stop = OpenEventW(
      EVENT_MODIFY_STATE, FALSE, soundfork::capture::kRelayStopEventName);
  if (stop != nullptr) {
    (void)SetEvent(stop);
    CloseHandle(stop);
  }
  if (g_worker.joinable()) {
    g_worker.join();
  }
  g_running.store(false, std::memory_order_release);
  return ERROR_SUCCESS;
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI
SoundForkAppRelayStart(const wchar_t* endpoint_id) noexcept {
  if (endpoint_id == nullptr || endpoint_id[0] == L'\0') {
    return ERROR_INVALID_PARAMETER;
  }
  if (g_running.exchange(true, std::memory_order_acq_rel)) {
    return ERROR_ALREADY_EXISTS;
  }
  if (g_worker.joinable()) {
    g_worker.join();
  }
  HANDLE existing = OpenMutexW(
      SYNCHRONIZE, FALSE, soundfork::capture::kRelaySingletonName);
  if (existing != nullptr) {
    CloseHandle(existing);
    g_running.store(false, std::memory_order_release);
    return ERROR_ALREADY_EXISTS;
  }

  try {
    const std::wstring endpoint(endpoint_id);
    g_worker = std::thread([endpoint]() {
      std::vector<std::wstring> arguments{
          L"soundfork_app_relay.dll", L"--endpoint-id", endpoint,
          L"--system-share", L"--persistent"};
      std::vector<wchar_t*> argv;
      argv.reserve(arguments.size());
      for (auto& argument : arguments) {
        argv.push_back(argument.data());
      }
      const int result = soundfork_relay_main(
          static_cast<int>(argv.size()), argv.data());
      soundfork::diagnostics::log().write(
          result == 0 ? soundfork::diagnostics::Level::info
                      : soundfork::diagnostics::Level::error,
          "app_relay_exit", "exit_code=" + std::to_string(result));
      soundfork::diagnostics::log().stop();
      g_running.store(false, std::memory_order_release);
    });
  } catch (...) {
    g_running.store(false, std::memory_order_release);
    return ERROR_NOT_ENOUGH_MEMORY;
  }
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI
SoundForkAppRelayStop() noexcept {
  return stop_worker();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  // The owner must call SoundForkAppRelayStop before FreeLibrary. Performing
  // thread or COM work under the loader lock would be unsafe.
  (void)reason;
  return TRUE;
}

