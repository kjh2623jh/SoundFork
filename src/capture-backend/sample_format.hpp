#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace soundfork::capture {

// Stable wire IDs, deliberately independent of ASIO's enum values.
enum class SampleFormat : std::uint32_t {
  invalid = 0, s16le = 1, s24le = 2, s32le = 3, f32le = 4,
  f64le = 5, s16be = 6, s24be = 7, s32be = 8, f32be = 9, f64be = 10,
  s32le16 = 11, s32le18 = 12, s32le20 = 13, s32le24 = 14,
  s32be16 = 15, s32be18 = 16, s32be20 = 17, s32be24 = 18
};
enum class ChannelLayout : std::uint32_t { planar = 1 };

constexpr std::uint32_t sample_bytes(SampleFormat format) noexcept {
  const auto id = static_cast<std::uint32_t>(format);
  if(id == 5 || id == 10) return 8;
  if(id == 6) return 2;
  if(id == 7) return 3;
  if(id >= 8 && id <= 18) return 4;
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
    case SampleFormat::f64le: return "Float64LSB";
    case SampleFormat::s16be: return "Int16MSB";
    case SampleFormat::s24be: return "Int24MSB";
    case SampleFormat::s32be: return "Int32MSB";
    case SampleFormat::f32be: return "Float32MSB";
    case SampleFormat::f64be: return "Float64MSB";
    case SampleFormat::s32le16: return "Int32LSB16";
    case SampleFormat::s32le18: return "Int32LSB18";
    case SampleFormat::s32le20: return "Int32LSB20";
    case SampleFormat::s32le24: return "Int32LSB24";
    case SampleFormat::s32be16: return "Int32MSB16";
    case SampleFormat::s32be18: return "Int32MSB18";
    case SampleFormat::s32be20: return "Int32MSB20";
    case SampleFormat::s32be24: return "Int32MSB24";
    default: return "unsupported";
  }
}
constexpr bool float_format(SampleFormat format) noexcept {
  return format == SampleFormat::f32le || format == SampleFormat::f32be ||
      format == SampleFormat::f64le || format == SampleFormat::f64be;
}
constexpr bool big_endian(SampleFormat format) noexcept {
  const auto id = static_cast<std::uint32_t>(format);
  return (id >= 6 && id <= 10) || (id >= 15 && id <= 18);
}
constexpr unsigned valid_bits(SampleFormat format) noexcept {
  const auto id = static_cast<unsigned>(format);
  constexpr unsigned bits[]{16,18,20,24};
  if(id >= 11 && id <= 18) return bits[(id - 11) % 4];
  return sample_bytes(format) * 8;
}
inline std::uint64_t sample_word(const std::byte* source, SampleFormat format) noexcept {
  std::uint64_t word = 0; const auto bytes = sample_bytes(format);
  for(unsigned i = 0; i < bytes; ++i) {
    const auto shift = (big_endian(format) ? bytes - 1 - i : i) * 8;
    word |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(source[i])) << shift;
  }
  return word;
}
inline double decode_floating(const std::byte* source, SampleFormat format) noexcept {
  const auto word = sample_word(source,format);
  const double value = sample_bytes(format) == 8 ? std::bit_cast<double>(word)
      : static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(word)));
  return std::isfinite(value) ? value : 0.0;
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
    default: {
      if(sample_bytes(format) == 0) return 0;
      if(float_format(format)) {
        const double value = (std::clamp)(decode_floating(source,format),-1.0,1.0);
        return static_cast<std::int32_t>((std::clamp)(std::round(value * 2147483648.0),-2147483648.0,2147483647.0));
      }
      const auto bits = valid_bits(format);
      if(bits == 0 || bits > 32) return 0;
      const auto word = sample_word(source,format) & ((1ULL << bits) - 1);
      const auto signed_value = (word & (1ULL << (bits - 1)))
          ? static_cast<std::int64_t>(word) - (1LL << bits) : static_cast<std::int64_t>(word);
      return static_cast<std::int32_t>(signed_value * (1LL << (32 - bits)));
    }
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
        if(format == SampleFormat::f32le) output[static_cast<std::size_t>(frame)*2+channel] = finite_float(sample);
        else if(float_format(format)) {
          const auto limit = static_cast<double>((std::numeric_limits<float>::max)());
          output[static_cast<std::size_t>(frame)*2+channel] = static_cast<float>((std::clamp)(decode_floating(sample,format),-limit,limit));
        } else output[static_cast<std::size_t>(frame)*2+channel] = static_cast<float>(static_cast<double>(decode_int32(sample,format))/2147483648.0);
      } else {
        output[static_cast<std::size_t>(frame) * 2 + channel] = decode_int32(sample, format);
      }
    }
  }
  return true;
}
}  // namespace soundfork::capture
