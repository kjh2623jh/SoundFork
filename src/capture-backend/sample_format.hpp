#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace soundfork::capture {

// Stable wire IDs, deliberately independent of ASIO's enum values.
enum class SampleFormat : std::uint32_t { invalid = 0, s16le = 1, s24le = 2, s32le = 3, f32le = 4 };
enum class ChannelLayout : std::uint32_t { planar = 1 };

constexpr std::uint32_t sample_bytes(SampleFormat format) noexcept {
  switch (format) {
    case SampleFormat::s16le: return 2;
    case SampleFormat::s24le: return 3;
    case SampleFormat::s32le:
    case SampleFormat::f32le: return 4;
    default: return 0;
  }
}

constexpr const char* sample_format_name(SampleFormat format) noexcept {
  switch (format) {
    case SampleFormat::s16le: return "Int16LSB";
    case SampleFormat::s24le: return "Int24LSB";
    case SampleFormat::s32le: return "Int32LSB";
    case SampleFormat::f32le: return "Float32LSB";
    default: return "unsupported";
  }
}

inline float finite_float(const void* source) noexcept {
  float value = 0;
  std::memcpy(&value, source, sizeof(value));
  return std::isfinite(value) ? value : 0.0F;
}

inline std::int32_t decode_int32(const std::byte* source, SampleFormat format) noexcept {
  switch (format) {
    case SampleFormat::s16le: {
      std::int16_t value = 0;
      std::memcpy(&value, source, sizeof(value));
      return static_cast<std::int32_t>(value) * 65536;
    }
    case SampleFormat::s24le: {
      const auto* bytes = reinterpret_cast<const unsigned char*>(source);
      const std::int32_t value = static_cast<std::int32_t>(bytes[0]) |
          (static_cast<std::int32_t>(bytes[1]) << 8) |
          (static_cast<std::int32_t>(bytes[2]) << 16);
      return (value >= 0x800000 ? value - 0x1000000 : value) * 256;
    }
    case SampleFormat::s32le: {
      std::int32_t value = 0;
      std::memcpy(&value, source, sizeof(value));
      return value;
    }
    case SampleFormat::f32le: {
      const double value = (std::clamp)(static_cast<double>(finite_float(source)), -1.0, 1.0);
      const double rounded = std::round(value * 2147483648.0);
      return static_cast<std::int32_t>((std::clamp)(rounded, -2147483648.0, 2147483647.0));
    }
    default: return 0;
  }
}

// Consumer-side only. Input may be unaligned, including packed 24-bit samples.
template <typename Sample>
bool decode_planar(SampleFormat format, std::uint32_t frames, const void* payload,
                   std::size_t payload_bytes, Sample* output, std::size_t sample_capacity) noexcept {
  static_assert(std::is_same_v<Sample, float> || std::is_same_v<Sample, std::int32_t>);
  const auto bytes = sample_bytes(format);
  if (bytes == 0 || frames == 0 || payload == nullptr || output == nullptr ||
      frames > sample_capacity / 2 || frames > payload_bytes / (2 * bytes)) return false;
  const auto* source = static_cast<const std::byte*>(payload);
  const std::size_t plane_bytes = static_cast<std::size_t>(frames) * bytes;
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    for (std::uint32_t channel = 0; channel < 2; ++channel) {
      const auto* sample = source + channel * plane_bytes + static_cast<std::size_t>(frame) * bytes;
      if constexpr (std::is_same_v<Sample, float>) {
        output[static_cast<std::size_t>(frame) * 2 + channel] = format == SampleFormat::f32le
            ? finite_float(sample) : static_cast<float>(static_cast<double>(decode_int32(sample, format)) / 2147483648.0);
      } else {
        output[static_cast<std::size_t>(frame) * 2 + channel] = decode_int32(sample, format);
      }
    }
  }
  return true;
}
}  // namespace soundfork::capture
