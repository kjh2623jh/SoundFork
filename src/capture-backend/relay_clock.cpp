#include "capture-backend/relay_clock.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace soundfork::capture {
namespace {

constexpr double kMaximumCorrectionPpm = 2000.0;
constexpr double kProportionalPpmPerFrame = 6.0;
constexpr double kIntegralPpmPerFrameSecond = 0.05;

std::int32_t interpolate(
    std::int32_t first, std::int32_t second, double phase) noexcept {
  const double value = static_cast<double>(first) +
                       (static_cast<double>(second) - first) * phase;
  const double bounded = (std::clamp)(
      value,
      static_cast<double>((std::numeric_limits<std::int32_t>::min)()),
      static_cast<double>((std::numeric_limits<std::int32_t>::max)()));
  return static_cast<std::int32_t>(std::llround(bounded));
}

}  // namespace

RelayClockController::RelayClockController(
    std::uint32_t sample_rate,
    std::uint32_t frames_per_block,
    std::uint32_t target_blocks) noexcept
    : sample_rate_(sample_rate),
      target_frames_(
          static_cast<std::uint64_t>(frames_per_block) * target_blocks) {}

void RelayClockController::reset() noexcept {
  integral_ppm_ = 0.0;
  correction_ppm_ = 0.0;
}

double RelayClockController::update(
    std::uint64_t buffered_frames,
    std::uint32_t rendered_frames) noexcept {
  if (sample_rate_ == 0 || target_frames_ == 0) {
    correction_ppm_ = 0.0;
    return 1.0;
  }
  const double error = static_cast<double>(buffered_frames) -
                       static_cast<double>(target_frames_);
  const double elapsed =
      static_cast<double>(rendered_frames) / static_cast<double>(sample_rate_);
  integral_ppm_ += error * kIntegralPpmPerFrameSecond * elapsed;
  integral_ppm_ = (std::clamp)(
      integral_ppm_, -kMaximumCorrectionPpm, kMaximumCorrectionPpm);
  correction_ppm_ = (std::clamp)(
      error * kProportionalPpmPerFrame + integral_ppm_,
      -kMaximumCorrectionPpm,
      kMaximumCorrectionPpm);
  return 1.0 + correction_ppm_ * 0.000001;
}

std::uint64_t RelayClockController::target_frames() const noexcept {
  return target_frames_;
}

double RelayClockController::correction_ppm() const noexcept {
  return correction_ppm_;
}

RelayAudioSource::RelayAudioSource(const StreamFormat& format) noexcept
    : format_(format) {}

void RelayAudioSource::reset() noexcept {
  block_frames_ = 0;
  block_offset_ = 0;
  first_ = {};
  second_ = {};
  have_pair_ = false;
  phase_ = 0.0;
}

std::uint64_t RelayAudioSource::buffered_frames(
    const SharedRingReader& reader) const noexcept {
  const std::uint64_t queued =
      reader.queued_blocks() * format_.frames_per_block;
  const std::uint64_t local = block_frames_ > block_offset_
                                  ? block_frames_ - block_offset_
                                  : 0;
  return queued + local + (have_pair_ ? 2U : 0U);
}

bool RelayAudioSource::take_source_frame(
    SharedRingReader& reader,
    std::array<std::int32_t, 2>* frame) noexcept {
  if (frame == nullptr) {
    return false;
  }
  if (block_offset_ >= block_frames_) {
    block_offset_ = 0;
    block_frames_ = 0;
    if (!reader.read_int32_stereo(
            block_.data(), block_.size(), &block_frames_)) {
      return false;
    }
  }
  (*frame)[0] = block_[block_offset_ * 2];
  (*frame)[1] = block_[block_offset_ * 2 + 1];
  ++block_offset_;
  return true;
}

bool RelayAudioSource::ensure_pair(SharedRingReader& reader) noexcept {
  if (have_pair_) {
    return true;
  }
  if (!take_source_frame(reader, &first_) ||
      !take_source_frame(reader, &second_)) {
    reset();
    return false;
  }
  have_pair_ = true;
  return true;
}

bool RelayAudioSource::render(
    SharedRingReader& reader,
    std::int32_t* destination,
    std::uint32_t frames,
    double source_frames_per_output_frame) noexcept {
  if (destination == nullptr || source_frames_per_output_frame <= 0.0) {
    return false;
  }
  bool any_audio = false;
  for (std::uint32_t output = 0; output < frames; ++output) {
    if (!ensure_pair(reader)) {
      destination[output * 2] = 0;
      destination[output * 2 + 1] = 0;
      ++metrics_.silent_frames;
      continue;
    }
    destination[output * 2] = interpolate(first_[0], second_[0], phase_);
    destination[output * 2 + 1] =
        interpolate(first_[1], second_[1], phase_);
    any_audio = true;
    phase_ += source_frames_per_output_frame;
    while (phase_ >= 1.0) {
      first_ = second_;
      phase_ -= 1.0;
      ++metrics_.source_frames;
      if (!take_source_frame(reader, &second_)) {
        have_pair_ = false;
        phase_ = 0.0;
        break;
      }
    }
  }
  return any_audio;
}

RelaySourceMetrics RelayAudioSource::metrics() const noexcept {
  return metrics_;
}

}  // namespace soundfork::capture
