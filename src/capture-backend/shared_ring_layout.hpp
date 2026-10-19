#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>

#include "capture-backend/shared_ring.hpp"

namespace soundfork::capture::detail {

inline constexpr std::uint32_t kMagic = 0x4B524653;  // "SFRK"
inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::size_t kHeaderBytes = 256;
inline constexpr std::size_t kSamplesPerBlock =
    static_cast<std::size_t>(kMaxFramesPerBlock) * kChannelCount;
inline constexpr std::size_t kDataBytes =
    kSamplesPerBlock * sizeof(std::int32_t) * kCapacityBlocks;
inline constexpr std::size_t kMappingBytes = kHeaderBytes + kDataBytes;

struct SharedHeader {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t header_bytes;
  std::uint32_t mapping_bytes;
  std::uint32_t format_checksum;
  std::uint32_t sample_rate;
  std::uint32_t channel_count;
  std::uint32_t frames_per_block;
  std::uint32_t bytes_per_sample;
  std::uint32_t capacity_blocks;
  std::uint32_t max_frames_per_block;
  std::uint32_t producer_pid;
  std::uint32_t consumer_pid;
  volatile LONG producer_active;
  volatile LONG consumer_active;
  volatile LONG ready;
  alignas(8) volatile LONG64 generation;
  alignas(8) volatile LONG64 write_sequence;
  alignas(8) volatile LONG64 read_sequence;
  alignas(8) volatile LONG64 dropped_blocks;
};

constexpr std::uint32_t format_checksum(
    std::uint32_t sample_rate, std::uint32_t frames_per_block) noexcept {
  return kMagic ^ kVersion ^ static_cast<std::uint32_t>(kHeaderBytes) ^
         static_cast<std::uint32_t>(kMappingBytes) ^ sample_rate ^
         kChannelCount ^ frames_per_block ^ sizeof(std::int32_t) ^
         kCapacityBlocks ^ kMaxFramesPerBlock;
}

constexpr std::size_t block_offset(std::size_t slot) noexcept {
  return kHeaderBytes + slot * kSamplesPerBlock * sizeof(std::int32_t);
}

static_assert(sizeof(SharedHeader) <= kHeaderBytes);
static_assert(kHeaderBytes % alignof(std::int32_t) == 0);
static_assert(kMappingBytes <= std::numeric_limits<DWORD>::max());
static_assert(block_offset(kCapacityBlocks) == kMappingBytes);

}  // namespace soundfork::capture::detail
