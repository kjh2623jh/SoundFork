#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "capture-backend/shared_ring.hpp"

namespace soundfork::capture {

class RelayClockController final {
 public:
  RelayClockController(
      std::uint32_t sample_rate,
      std::uint32_t frames_per_block,
      std::uint32_t target_blocks = 4) noexcept;

  void reset() noexcept;
  double update(
      std::uint64_t buffered_frames,
      std::uint32_t rendered_frames) noexcept;
  std::uint64_t target_frames() const noexcept;
  double correction_ppm() const noexcept;

 private:
  std::uint32_t sample_rate_ = 0;
  std::uint64_t target_frames_ = 0;
  double integral_ppm_ = 0.0;
  double correction_ppm_ = 0.0;
};

struct RelaySourceMetrics {
  std::uint64_t source_frames = 0;
  std::uint64_t silent_frames = 0;
};

template <typename Sample>
class RelaySampleSource final {
 public:
  explicit RelaySampleSource(const StreamFormat& format) noexcept;

  void reset() noexcept;
  std::uint64_t buffered_frames(const SharedRingReader& reader) const noexcept;
  bool render(
      SharedRingReader& reader,
      Sample* destination,
      std::uint32_t frames,
      double source_frames_per_output_frame) noexcept;
  RelaySourceMetrics metrics() const noexcept;

 private:
  bool take_source_frame(
      SharedRingReader& reader,
      std::array<Sample, 2>* frame) noexcept;
  bool ensure_pair(SharedRingReader& reader) noexcept;

  StreamFormat format_{};
  std::array<Sample,
             kMaxFramesPerBlock * kChannelCount>
      block_{};
  std::uint32_t block_frames_ = 0;
  std::uint32_t block_offset_ = 0;
  std::array<Sample, 2> first_{};
  std::array<Sample, 2> second_{};
  bool have_pair_ = false;
  double phase_ = 0.0;
  RelaySourceMetrics metrics_{};
};

using RelayAudioSource = RelaySampleSource<std::int32_t>;
using RelayFloatAudioSource = RelaySampleSource<float>;
extern template class RelaySampleSource<std::int32_t>;
extern template class RelaySampleSource<float>;

}  // namespace soundfork::capture
