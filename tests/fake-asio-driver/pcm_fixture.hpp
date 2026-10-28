#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "audio-bridge/backend_factory.hpp"
#include "capture-backend/sample_format.hpp"

namespace soundfork::test {
struct PcmCase {
  ASIOSampleType asio;
  capture::SampleFormat wire;
  const wchar_t* name;
  std::uint32_t bytes;
};
inline constexpr std::array<PcmCase, 4> kPcmCases{{
    {ASIOSTInt16LSB, capture::SampleFormat::s16le, L"int16", 2},
    {ASIOSTInt24LSB, capture::SampleFormat::s24le, L"int24", 3},
    {ASIOSTInt32LSB, capture::SampleFormat::s32le, L"int32", 4},
    {ASIOSTFloat32LSB, capture::SampleFormat::f32le, L"float32", 4}}};

// Test-only encoder; deliberately does not use the production PCM decoder.
// Each callback, channel and frame has a changing, exactly representable value.
inline void fill_pcm(void* buffer, const PcmCase& format, std::uint32_t frames,
                     std::uint64_t block, std::uint32_t channel) noexcept {
  auto* output = static_cast<std::byte*>(buffer);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto value = static_cast<std::int32_t>((block * 17 + frame * 13ULL + channel * 31) & 0x3fff) - 8192;
    auto* sample = output + static_cast<std::size_t>(frame) * format.bytes;
    switch (format.asio) {
      case ASIOSTInt16LSB: {
        const auto narrow = static_cast<std::int16_t>(value);
        std::memcpy(sample, &narrow, 2); break;
      }
      case ASIOSTInt24LSB: {
        const auto packed = static_cast<std::uint32_t>(value * 256);
        for (unsigned i = 0; i < 3; ++i) sample[i] = static_cast<std::byte>((packed >> (i * 8)) & 255);
        break;
      }
      case ASIOSTInt32LSB: {
        const std::int32_t wide = value * 65536;
        std::memcpy(sample, &wide, 4); break;
      }
      case ASIOSTFloat32LSB: {
        const float floating = static_cast<float>(value) / 16384.0F;
        std::memcpy(sample, &floating, 4); break;
      }
      default: break;
    }
  }
}
}  // namespace soundfork::test
