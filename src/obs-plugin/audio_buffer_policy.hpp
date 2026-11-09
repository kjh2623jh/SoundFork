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
  explicit AudioBufferPolicy(std::uint32_t block_frames, std::uint32_t sample_rate = 48000) noexcept
      : block_frames_(block_frames),
        packet_frames_(sample_rate / 100),
        target_blocks_(block_frames == 0 ? 0 : (std::max)(4U, (2 * packet_frames_ + block_frames - 1) / block_frames)) {}

  std::uint32_t target_blocks() const noexcept { return target_blocks_; }
  std::uint64_t target_frames() const noexcept {
    return static_cast<std::uint64_t>(block_frames_) * target_blocks_;
  }
  std::uint64_t startup_frames() const noexcept {
    return target_frames() + packet_frames_ + 2;
  }
  std::uint64_t high_water_frames() const noexcept {
    // Preserve the existing <=48kHz policy. New higher-rate streams need
    // enough ring time for a delayed producer burst, not just more samples.
    return (std::max)(static_cast<std::uint64_t>(block_frames_) * 48,
                      packet_frames_ > 480 ? static_cast<std::uint64_t>(packet_frames_) * 6 : 0ULL);
  }
  bool supported() const noexcept {
    return block_frames_ != 0 && block_frames_ <= capture::kMaxFramesPerBlock &&
           startup_frames() < high_water_frames() &&
           high_water_frames() < static_cast<std::uint64_t>(block_frames_) * capture::kCapacityBlocks;
  }
  std::uint64_t keep_blocks() const noexcept {
    return block_frames_ == 0 ? 0 : (startup_frames() + block_frames_ - 1) / block_frames_;
  }
  bool ready(std::uint64_t queued) noexcept {
    if (!ready_ && queued >= startup_frames()) ready_ = true;
    return ready_;
  }
  // A brief upstream stall need not become an audible zero-filled packet.
  // Permit bounded, timestamp-preserving late delivery; never spin or wait on
  // the ASIO producer. The OBS worker retries with its one-shot timer.
  bool wait_for_input(std::uint64_t queued, std::uint64_t now) noexcept {
    const auto required = packet_frames_ + (packet_frames_ + 499) / 500 + 2;
    if (!ready_ || queued >= required) {
      waiting_ = false;
      return false;
    }
    if (!waiting_) {
      waiting_ = true;
      wait_started_ = now;
    }
    return now - wait_started_ < 20000000;
  }
  void reset() noexcept { ready_ = false; waiting_ = false; }

 private:
  std::uint32_t block_frames_;
  std::uint32_t packet_frames_;
  std::uint32_t target_blocks_;
  bool ready_ = false;
  bool waiting_ = false;
  std::uint64_t wait_started_ = 0;
};

}  // namespace soundfork::obs
