#pragma once
#include <windows.h>
#include <algorithm>

namespace soundfork {
// Reject invalid driver metadata, and round up rather than requesting an illegal period.
constexpr UINT32 minimum_shared_period(UINT32 fundamental, UINT32 minimum, UINT32 maximum) noexcept {
  if (!fundamental || !minimum || minimum > maximum || maximum > 96000) return 0;
  const auto rounded = (static_cast<UINT64>(minimum) + fundamental - 1) / fundamental * fundamental;
  return rounded <= maximum ? static_cast<UINT32>(rounded) : 0;
}
constexpr UINT32 shared_queue_target(UINT32 capacity, UINT32 period) noexcept {
  // Two engine periods absorb scheduling jitter without filling an oversized endpoint buffer.
  return period ? static_cast<UINT32>((std::min)(static_cast<UINT64>(capacity), 2ULL * period)) : capacity;
}
constexpr UINT32 shared_write_frames(UINT32 capacity, UINT32 target, UINT32 padding) noexcept {
  if (padding >= capacity || padding >= target) return 0;
  return (std::min)(capacity - padding, target - padding);
}
// The fallback callback must activate a fresh audio client after any failed initialization.
template<class Query, class LowLatency, class Legacy>
HRESULT initialize_shared_render(Query query, LowLatency low_latency, Legacy legacy,
    UINT32* selected_period, HRESULT* low_latency_result) {
  *selected_period = 0;
  UINT32 fundamental = 0, minimum = 0, maximum = 0;
  auto result = query(&fundamental, &minimum, &maximum);
  if (SUCCEEDED(result)) {
    const auto period = minimum_shared_period(fundamental, minimum, maximum);
    result = period ? low_latency(period) : E_INVALIDARG;
    if (SUCCEEDED(result)) {
      *selected_period = period; *low_latency_result = result; return result;
    }
  }
  *low_latency_result = result;
  return legacy();
}
} // namespace soundfork
