#pragma once

#include <algorithm>
#include <cstdint>
#include "capture-backend/relay_clock.hpp"

namespace soundfork::capture {

enum class SourcePoll { tick, timeout, stopped };
struct SourceWaitResult {
  std::uint64_t queued = 0;
  unsigned polls = 0;
  bool recovered = false;
  bool stopped = false;
};

inline std::uint64_t source_wait_budget_us(std::uint32_t padding, std::uint32_t rate) noexcept {
  if (!rate) return 0;
  const auto device_time = static_cast<std::uint64_t>(padding) * 1000000 / rate;
  // Keep at least 1ms of device audio as a guard. Never wait for initial fill.
  return device_time > 1000 ? (std::min)(std::uint64_t{2000}, device_time - 1000) : 0;
}

// The wait callback must yield, honor this session's deadline and stop event,
// and run before acquiring a WASAPI render buffer. No ASIO thread calls this.
template <typename Read, typename Wait>
SourceWaitResult wait_for_relay_source(const RelayRenderClock& clock,
    std::uint64_t queued, std::uint32_t output, std::uint64_t budget_us,
    Read read, Wait wait) {
  SourceWaitResult result{queued};
  const auto polls = (std::min)(std::uint64_t{2}, budget_us / 1000);
  if (!clock.needs_source(queued, output)) return result;
  for (std::uint64_t poll = 0; poll < polls; ++poll) {
    ++result.polls;
    const auto status = wait();
    switch (status) {
      case SourcePoll::stopped: result.stopped = true; return result;
      case SourcePoll::timeout:
        // A late timeout wake may coincide with newly delivered source data.
        result.queued = read();
        result.recovered = !clock.needs_source(result.queued, output);
        return result;
      case SourcePoll::tick: break;
    }
    result.queued = read();
    if (!clock.needs_source(result.queued, output)) { result.recovered = true; break; }
  }
  return result;
}

}  // namespace soundfork::capture
