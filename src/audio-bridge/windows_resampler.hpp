#pragma once
#include <array>
#include <vector>
#include <cmath>
#include <numeric>
#include <cstdint>
#include <algorithm>

namespace soundfork {
// Causal stereo polyphase FIR. All coefficient allocation occurs in prepare().
// Input is always 48 kHz. Processing is independent of ASIO block boundaries.
class WindowsResampler {
 public:
  static constexpr unsigned taps = 64;
  bool prepare(unsigned rate) {
    if(rate < 32000 || rate > 384000) return false;
    rate_ = rate; quantum_ = std::gcd(48000U, rate); phases_ = rate / quantum_;
    if(phases_ > 4096) return false;
    coefficients_.resize(static_cast<std::size_t>(phases_) * taps);
    const double cutoff = 0.94 * (std::min)(1.0, static_cast<double>(rate) / 48000.0);
    constexpr double pi = 3.14159265358979323846;
    for(unsigned phase = 0; phase < phases_; ++phase) {
      const double fraction = static_cast<double>(phase) / phases_;
      double sum = 0;
      for(unsigned k = 0; k < taps; ++k) {
        const double x = static_cast<double>(k) + fraction - (taps - 1) * 0.5;
        const double sinc = std::abs(x) < 1e-12 ? cutoff : std::sin(pi * cutoff * x) / (pi * x);
        const double window = 0.42 - 0.5 * std::cos(2 * pi * k / (taps - 1)) +
            0.08 * std::cos(4 * pi * k / (taps - 1));
        const auto value = sinc * window;
        coefficients_[static_cast<std::size_t>(phase) * taps + k] = static_cast<float>(value); sum += value;
      }
      for(unsigned k = 0; k < taps; ++k)
        coefficients_[static_cast<std::size_t>(phase) * taps + k] /= static_cast<float>(sum);
    }
    reset(); return true;
  }
  void reset() noexcept { phase_ = 0; primed_ = false; cursor_ = 0; history_ = {}; }
  unsigned input_frames(unsigned output) const noexcept {
    if(!output) return 0;
    return static_cast<unsigned>((phase_ + static_cast<std::uint64_t>(output - 1) * 48000) / rate_) + (primed_ ? 0U : 1U);
  }
  void process(const float* input, unsigned output_frames, float* output) noexcept {
    unsigned consumed = 0;
    for(unsigned frame = 0; frame < output_frames; ++frame) {
      if(!primed_) { push(input, consumed++); primed_ = true; }
      while(phase_ >= rate_) { push(input, consumed++); phase_ -= rate_; }
      const auto* filter = coefficients_.data() + static_cast<std::size_t>(phase_ / quantum_) * taps;
      double left = 0, right = 0;
      for(unsigned k = 0; k < taps; ++k) {
        const auto& sample = history_[(cursor_ + taps - 1 - k) % taps];
        left += sample[0] * filter[k]; right += sample[1] * filter[k];
      }
      output[frame * 2] = static_cast<float>(left); output[frame * 2 + 1] = static_cast<float>(right);
      phase_ += 48000;
    }
  }
 private:
  void push(const float* input, unsigned frame) noexcept {
    history_[cursor_] = {input[frame * 2], input[frame * 2 + 1]}; cursor_ = (cursor_ + 1) % taps;
  }
  unsigned rate_ = 48000, quantum_ = 48000, phases_ = 1, cursor_ = 0;
  std::uint64_t phase_ = 0;
  bool primed_ = false;
  std::vector<float> coefficients_;
  std::array<std::array<float, 2>, taps> history_{};
};
} // namespace soundfork
