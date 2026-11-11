// Isolated diagnostic: compile the production application-relay lifecycle with
// a delayed fake worker and private handles. No installed relay, device, named
// SoundFork event, hub, registry setting or diagnostic file is touched.
#include <windows.h>
#include <atomic>
#include <iostream>
#include <thread>
#include "capture-backend/relay_stop.hpp"

namespace probe {
HANDLE entered = nullptr;
HANDLE begin = nullptr;
HANDLE stop_attempted = nullptr;
HANDLE private_stop = nullptr;
std::atomic<bool> ready{false};
std::atomic<bool> timed_out{false};

HANDLE WINAPI open_mutex(DWORD, BOOL, LPCWSTR) { return nullptr; }
BOOL WINAPI signal_stop(HANDLE event) {
  SetEvent(stop_attempted);
  return SetEvent(event);
}
}  // namespace probe

#define OpenMutexW probe::open_mutex
#define SetEvent probe::signal_stop
#include "../src/capture-backend/app_relay.cpp"
#undef SetEvent
#undef OpenMutexW

int soundfork_relay_main(int, wchar_t**, HANDLE owned_stop) {
  SetEvent(probe::entered);
  if (WaitForSingleObject(probe::begin, 5000) != WAIT_OBJECT_0) return 2;
  probe::ready.store(true, std::memory_order_release);
  // The real persistent worker has no such deadline. The probe's deadline
  // guarantees that reproducing a lost stop request cannot hang the test.
  probe::timed_out.store(
      WaitForSingleObject(owned_stop, 1000) != WAIT_OBJECT_0,
      std::memory_order_release);
  return 0;
}

int main() {
  HANDLE shared = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE owned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE audio = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  if (!shared || !owned || !audio) return 2;
  soundfork::capture::RelayStop both(shared, owned);
  soundfork::capture::RelayStop external(shared, nullptr);
  bool waits_valid = both.wait(0) == WAIT_TIMEOUT &&
      both.wait_with(audio, 0) == WAIT_OBJECT_0 + 1 &&
      external.wait_with(audio, 0) == WAIT_OBJECT_0 + 1;
  SetEvent(owned);
  waits_valid = waits_valid && both.wait(0) == WAIT_OBJECT_0 &&
      both.wait_with(audio, 0) == WAIT_OBJECT_0 &&
      external.wait(0) == WAIT_TIMEOUT;
  SetEvent(shared);
  waits_valid = waits_valid && external.wait_with(audio, 0) == WAIT_OBJECT_0;
  CloseHandle(audio); CloseHandle(owned); CloseHandle(shared);
  if (!waits_valid) return 3;
  probe::entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  probe::begin = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  probe::stop_attempted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  probe::private_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!probe::entered || !probe::begin || !probe::stop_attempted ||
      !probe::private_stop) return 2;
  if (SoundForkAppRelayStart(L"isolated-fake-endpoint") != ERROR_SUCCESS)
    return 2;
  if (WaitForSingleObject(probe::entered, 5000) != WAIT_OBJECT_0) {
    SetEvent(probe::begin);
    SetEvent(probe::private_stop);
    SoundForkAppRelayStop();
    return 2;
  }
  const ULONGLONG started = GetTickCount64();
  std::thread stopper([] { SoundForkAppRelayStop(); });
  const bool attempted =
      WaitForSingleObject(probe::stop_attempted, 5000) == WAIT_OBJECT_0;
  SetEvent(probe::begin);
  stopper.join();
  const auto elapsed = GetTickCount64() - started;
  const bool lost = probe::timed_out.load(std::memory_order_acquire);
  ResetEvent(probe::stop_attempted);
  SoundForkAppRelayStop();
  const bool inactive_signaled =
      WaitForSingleObject(probe::stop_attempted, 0) == WAIT_OBJECT_0;
  for (HANDLE handle : {probe::entered, probe::begin,
                        probe::stop_attempted, probe::private_stop})
    CloseHandle(handle);
  std::cout << "stop_attempted=" << attempted << " lost_stop=" << lost
            << " stop_elapsed_ms=" << elapsed << '\n';
  return attempted && !lost && !inactive_signaled ? 0 : 1;
}
