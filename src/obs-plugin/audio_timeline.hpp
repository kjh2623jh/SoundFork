#pragma once

#include <algorithm>
#include <cstdint>

namespace soundfork::obs {

// Timer notifications are wake-up hints, not audio-clock ticks. Keep this
// independent of Windows/OBS so delayed wakes can be tested with a virtual clock.
class AudioTimeline final {
 public:
  static constexpr std::uint64_t period_ns = 10000000;
  static constexpr std::uint32_t packet_frames = 480;
  static constexpr std::uint32_t maximum_batch = 5;
  static constexpr std::uint64_t maximum_lag_ns = 50000000;

  struct Batch {
    std::uint32_t packets = 0;
    bool resynchronized = false;
    std::uint64_t lateness_ns = 0;
  };

  Batch plan(std::uint64_t now) noexcept {
    if (!started_) {
      started_ = true;
      next_ = now;
    }
    if (now < next_) return {};
    const auto late = now - next_;
    const bool reset = late > maximum_lag_ns;
    if (reset) next_ = now;
    const auto count = (now - next_) / period_ns + 1;
    return {static_cast<std::uint32_t>((std::min)(
                count, static_cast<std::uint64_t>(maximum_batch))), reset, late};
  }

  std::uint64_t timestamp() const noexcept { return next_; }
  void advance() noexcept { next_ += period_ns; }

 private:
  bool started_ = false;
  std::uint64_t next_ = 0;
};

}  // namespace soundfork::obs
