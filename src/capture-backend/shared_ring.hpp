#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace soundfork::capture {

inline constexpr wchar_t kDefaultMappingName[] = L"Local\\SoundFork.Audio.v2";
inline constexpr std::uint32_t kChannelCount = 2;
inline constexpr std::uint32_t kCapacityBlocks = 64;
inline constexpr std::uint32_t kMaxFramesPerBlock = 8192;

struct StreamFormat {
  std::uint32_t sample_rate = 0;
  std::uint32_t channel_count = 0;
  std::uint32_t frames_per_block = 0;
  std::uint32_t bytes_per_sample = 0;
};

struct StreamStats {
  std::uint64_t published_blocks = 0;
  std::uint64_t consumed_blocks = 0;
  std::uint64_t dropped_blocks = 0;
};

struct StreamStatus {
  bool producer_active = false;
  bool consumer_active = false;
  std::uint32_t producer_pid = 0;
  std::uint32_t consumer_pid = 0;
  StreamFormat format{};
  StreamStats stats{};
};

class SharedRingPublisher final {
 public:
  SharedRingPublisher() = default;
  ~SharedRingPublisher();

  SharedRingPublisher(const SharedRingPublisher&) = delete;
  SharedRingPublisher& operator=(const SharedRingPublisher&) = delete;

  bool open(
      std::uint32_t sample_rate,
      std::uint32_t frames_per_block,
      const wchar_t* mapping_name = kDefaultMappingName) noexcept;
  void close() noexcept;
  bool publish_int32_stereo(
      const std::int32_t* left,
      const std::int32_t* right) noexcept;
  bool is_open() const noexcept;
  StreamStats stats() const noexcept;

 private:
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  bool owns_producer_ = false;
  StreamFormat format_{};
};

class SharedRingReader final {
 public:
  SharedRingReader() = default;
  ~SharedRingReader();

  SharedRingReader(const SharedRingReader&) = delete;
  SharedRingReader& operator=(const SharedRingReader&) = delete;

  bool open(
      const wchar_t* mapping_name = kDefaultMappingName,
      bool start_at_latest = true) noexcept;
  void close() noexcept;
  bool read_int32_stereo(
      std::int32_t* interleaved,
      std::size_t sample_capacity,
      std::uint32_t* frames_read) noexcept;
  bool is_open() const noexcept;
  StreamFormat format() const noexcept;
  StreamStats stats() const noexcept;
  std::uint64_t queued_blocks() const noexcept;
  std::uint64_t discard_oldest(std::uint64_t keep_blocks) noexcept;

 private:
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  bool owns_consumer_ = false;
  StreamFormat format_{};
};

// Read-only observer used by the control application. It never claims the
// single consumer slot and therefore cannot interfere with a relay or monitor.
class SharedRingObserver final {
 public:
  SharedRingObserver() = default;
  ~SharedRingObserver();

  SharedRingObserver(const SharedRingObserver&) = delete;
  SharedRingObserver& operator=(const SharedRingObserver&) = delete;

  bool open(const wchar_t* mapping_name = kDefaultMappingName) noexcept;
  void close() noexcept;
  bool snapshot(StreamStatus* status) const noexcept;
  bool is_open() const noexcept;

 private:
  HANDLE mapping_ = nullptr;
  const void* view_ = nullptr;
};

}  // namespace soundfork::capture
