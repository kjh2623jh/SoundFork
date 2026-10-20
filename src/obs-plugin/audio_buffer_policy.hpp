#pragma once

#include <algorithm>
#include <cstdint>

#include "capture-backend/shared_ring.hpp"
#include "obs-plugin/audio_timeline.hpp"

namespace soundfork::obs {

// OBS-only jitter headroom; the hardware playback and WASAPI relay retain
// their existing policies. Keep interpolation look-ahead as well as one packet.
class AudioBufferPolicy final {
 public:
  explicit AudioBufferPolicy(std::uint32_t block_frames) noexcept
      : block_frames_(block_frames),
        target_blocks_(block_frames == 0 ? 0 : (std::max)(4U, (960U + block_frames - 1) / block_frames)) {}

  std::uint32_t target_blocks() const noexcept { return target_blocks_; }
  std::uint64_t target_frames() const noexcept {
    return static_cast<std::uint64_t>(block_frames_) * target_blocks_;
  }
  std::uint64_t startup_frames() const noexcept {
    return target_frames() + AudioTimeline::packet_frames + 2;
  }
  std::uint64_t high_water_frames() const noexcept {
    return static_cast<std::uint64_t>(block_frames_) * 48;
  }
  bool supported() const noexcept {
    return block_frames_ != 0 && block_frames_ <= capture::kMaxFramesPerBlock &&
           startup_frames() < high_water_frames();
  }
  std::uint64_t keep_blocks() const noexcept {
    return block_frames_ == 0 ? 0 : (startup_frames() + block_frames_ - 1) / block_frames_;
  }
  bool ready(std::uint64_t queued) noexcept {
    if (!ready_ && queued >= startup_frames()) ready_ = true;
    return ready_;
  }
  void reset() noexcept { ready_ = false; }

 private:
  std::uint32_t block_frames_;
  std::uint32_t target_blocks_;
  bool ready_ = false;
};

}  // namespace soundfork::obs
