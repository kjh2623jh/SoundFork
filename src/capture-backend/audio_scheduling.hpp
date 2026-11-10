#pragma once

#include <windows.h>
#include <avrt.h>
#include <cstdint>

namespace soundfork::capture {

// Capture workers only; never attach this object to a host's ASIO callback.
class AudioScheduling final {
 public:
  AudioScheduling() = default;
  ~AudioScheduling() { reset(); }
  AudioScheduling(const AudioScheduling&) = delete;
  AudioScheduling& operator=(const AudioScheduling&) = delete;
  bool start() noexcept {
    if (handle_) return true;
    DWORD task = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    error_ = handle_ ? ERROR_SUCCESS : GetLastError();
    return handle_ != nullptr;
  }
  void reset() noexcept {
    if (handle_) AvRevertMmThreadCharacteristics(handle_);
    handle_ = nullptr;
  }
  DWORD error() const noexcept { return error_; }
 private:
  HANDLE handle_ = nullptr;
  DWORD error_ = ERROR_SUCCESS;
};

class WorkerTiming final {
 public:
  WorkerTiming() noexcept { QueryPerformanceFrequency(&frequency_); }
  std::uint64_t now_us() const noexcept {
    LARGE_INTEGER value{};
    if (frequency_.QuadPart <= 0 || !QueryPerformanceCounter(&value)) return 0;
    return static_cast<std::uint64_t>(static_cast<long double>(value.QuadPart) *
        1000000.0L / frequency_.QuadPart);
  }
  void observe_wake() noexcept {
    const auto now = now_us();
    if (previous_ && now >= previous_) {
      const auto gap = now - previous_;
      if (gap > maximum_gap_us) maximum_gap_us = gap;
    }
    previous_ = now;
  }
  void reset_interval() noexcept { previous_ = 0; }
  std::uint64_t maximum_gap_us = 0;
 private:
  LARGE_INTEGER frequency_{};
  std::uint64_t previous_ = 0;
};

}  // namespace soundfork::capture
