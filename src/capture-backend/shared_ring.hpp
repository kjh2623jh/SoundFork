#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "capture-backend/sample_format.hpp"

namespace soundfork::capture {

inline constexpr wchar_t kDefaultMappingName[] = L"Local\\SoundFork.Audio.v3";
inline constexpr std::uint32_t kChannelCount = 2;
inline constexpr std::uint32_t kCapacityBlocks = 64;
inline constexpr std::uint32_t kMaxFramesPerBlock = 8192;
inline constexpr std::size_t kMaxPayloadBytes = kMaxFramesPerBlock * kChannelCount * 4ULL;

enum class CaptureError : LONG {
  none = 0, buffer_size = 1, channels = 2, sample_format = 3,
  sample_rate = 4, format_changed = 5, backend_query = 6
};
constexpr const wchar_t* capture_error_text(CaptureError error, bool korean) noexcept {
  switch (error) {
    case CaptureError::buffer_size: return korean ? L"복제 미지원 버퍼 크기" : L"Unsupported capture block size";
    case CaptureError::channels: return korean ? L"출력 채널 0/1 필요" : L"Output channels 0/1 required";
    case CaptureError::sample_format: return korean ? L"미지원 또는 좌우 불일치 PCM 형식" : L"Unsupported or mismatched PCM formats";
    case CaptureError::sample_rate: return korean ? L"방송 복제는 48 kHz 필요" : L"Capture requires 48 kHz";
    case CaptureError::format_changed: return korean ? L"스트림 변경: 버퍼 재생성 필요" : L"Stream changed: recreate buffers";
    case CaptureError::backend_query: return korean ? L"ASIO 형식 조회 실패" : L"ASIO format query failed";
    default: return korean ? L"복제 상태 알 수 없음" : L"Unknown capture state";
  }
}

struct StreamFormat {
  std::uint32_t sample_rate = 0;
  std::uint32_t channel_count = 0;
  std::uint32_t frames_per_block = 0;
  std::uint32_t bytes_per_sample = 0;
  SampleFormat sample_format = SampleFormat::s32le;
  ChannelLayout layout = ChannelLayout::planar;
  std::uint32_t payload_bytes = 0;
};

struct StreamStats {
  std::uint64_t published_blocks = 0;
  std::uint64_t consumed_blocks = 0;
  std::uint64_t dropped_blocks = 0;
};

struct StreamStatus {
  CaptureError capture_error = CaptureError::none;
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
      const wchar_t* mapping_name = kDefaultMappingName,
      SampleFormat sample_format = SampleFormat::s32le,
      CaptureError status = CaptureError::none) noexcept;
  bool open_disabled(CaptureError reason, const wchar_t* mapping_name) noexcept;
  // Atomics only: safe for ASIO reset/rate notifications, never unmaps memory.
  void disable(CaptureError reason) noexcept;
  void close() noexcept;
  bool publish_int32_stereo(
      const std::int32_t* left,
      const std::int32_t* right) noexcept;
  bool publish_planar(const void* left, const void* right) noexcept;
  bool is_open() const noexcept;
  StreamStats stats() const noexcept;
  // Advisory presence flag for the hub. Reader::open handles stale owners.
  bool consumer_active() const noexcept;

 private:
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  bool owns_producer_ = false;
  StreamFormat format_{};
  std::int64_t generation_ = 0;
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
  bool read_float_stereo(float* interleaved, std::size_t sample_capacity,
                         std::uint32_t* frames_read) noexcept;
  bool read_planar(void* payload, std::size_t byte_capacity,
                   std::uint32_t* frames_read) noexcept;
  bool is_open() const noexcept;
  StreamFormat format() const noexcept;
  bool stream_active() const noexcept;
  StreamStats stats() const noexcept;
  std::uint64_t queued_blocks() const noexcept;
  std::uint64_t discard_oldest(std::uint64_t keep_blocks) noexcept;

 private:
  enum class ReadMode { raw, integer, floating };
  bool read_block(void* destination, std::size_t capacity, std::uint32_t* frames_read,
                  ReadMode mode) noexcept;
  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  bool owns_consumer_ = false;
  StreamFormat format_{};
  std::int64_t generation_ = 0;
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
