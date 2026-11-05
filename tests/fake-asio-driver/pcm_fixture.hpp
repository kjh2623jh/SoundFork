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
inline constexpr std::array<PcmCase, 18> kPcmCases{{
    {ASIOSTInt16LSB, capture::SampleFormat::s16le, L"int16", 2},
    {ASIOSTInt24LSB, capture::SampleFormat::s24le, L"int24", 3},
    {ASIOSTInt32LSB, capture::SampleFormat::s32le, L"int32", 4},
    {ASIOSTFloat32LSB, capture::SampleFormat::f32le, L"float32", 4},
    {ASIOSTFloat64LSB, capture::SampleFormat::f64le, L"float64", 8},
    {ASIOSTInt16MSB, capture::SampleFormat::s16be, L"int16be", 2},
    {ASIOSTInt24MSB, capture::SampleFormat::s24be, L"int24be", 3},
    {ASIOSTInt32MSB, capture::SampleFormat::s32be, L"int32be", 4},
    {ASIOSTFloat32MSB, capture::SampleFormat::f32be, L"float32be", 4},
    {ASIOSTFloat64MSB, capture::SampleFormat::f64be, L"float64be", 8},
    {ASIOSTInt32LSB16, capture::SampleFormat::s32le16, L"int32le16", 4},
    {ASIOSTInt32LSB18, capture::SampleFormat::s32le18, L"int32le18", 4},
    {ASIOSTInt32LSB20, capture::SampleFormat::s32le20, L"int32le20", 4},
    {ASIOSTInt32LSB24, capture::SampleFormat::s32le24, L"int32le24", 4},
    {ASIOSTInt32MSB16, capture::SampleFormat::s32be16, L"int32be16", 4},
    {ASIOSTInt32MSB18, capture::SampleFormat::s32be18, L"int32be18", 4},
    {ASIOSTInt32MSB20, capture::SampleFormat::s32be20, L"int32be20", 4},
    {ASIOSTInt32MSB24, capture::SampleFormat::s32be24, L"int32be24", 4}}};

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
      default: {
        std::array<std::byte,8> encoded{}; bool msb = false;
        if(format.asio == ASIOSTFloat64LSB || format.asio == ASIOSTFloat64MSB) {
          const double number = static_cast<double>(value) / 16384.0;
          std::memcpy(encoded.data(),&number,8); msb = format.asio == ASIOSTFloat64MSB;
        } else if(format.asio == ASIOSTFloat32MSB) {
          const float number = static_cast<float>(value) / 16384.0F;
          std::memcpy(encoded.data(),&number,4); msb = true;
        } else {
          unsigned bits = 32;
          switch(format.asio) {
            case ASIOSTInt16MSB: bits = 16; msb = true; break;
            case ASIOSTInt24MSB: bits = 24; msb = true; break;
            case ASIOSTInt32MSB: msb = true; break;
            case ASIOSTInt32LSB16: bits = 16; break;
            case ASIOSTInt32LSB18: bits = 18; break;
            case ASIOSTInt32LSB20: bits = 20; break;
            case ASIOSTInt32LSB24: bits = 24; break;
            case ASIOSTInt32MSB16: bits = 16; msb = true; break;
            case ASIOSTInt32MSB18: bits = 18; msb = true; break;
            case ASIOSTInt32MSB20: bits = 20; msb = true; break;
            case ASIOSTInt32MSB24: bits = 24; msb = true; break;
            default: return;
          }
          const auto number = static_cast<std::uint32_t>(value * (1 << (bits - 16)));
          for(unsigned b = 0; b < format.bytes; ++b) encoded[b] = static_cast<std::byte>((number >> (b*8)) & 255);
        }
        for(unsigned b = 0; b < format.bytes; ++b) sample[b] = encoded[msb ? format.bytes - 1 - b : b];
        break;
      }
    }
  }
}
}  // namespace soundfork::test
