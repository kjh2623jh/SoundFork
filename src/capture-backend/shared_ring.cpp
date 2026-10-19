#include "capture-backend/shared_ring.hpp"

#include <algorithm>
#include <cstring>

#include "capture-backend/shared_ring_layout.hpp"

namespace soundfork::capture {
namespace {

using detail::SharedHeader;
using detail::kDataBytes;
using detail::kHeaderBytes;
using detail::kMagic;
using detail::kMappingBytes;
using detail::kVersion;

SharedHeader* header(void* view) noexcept {
  return static_cast<SharedHeader*>(view);
}

const SharedHeader* header(const void* view) noexcept {
  return static_cast<const SharedHeader*>(view);
}

std::int32_t* data(void* view) noexcept {
  return reinterpret_cast<std::int32_t*>(
      static_cast<std::byte*>(view) + kHeaderBytes);
}

LONG64 load64(const volatile LONG64* value) noexcept {
  return InterlockedCompareExchange64(
      const_cast<volatile LONG64*>(value), 0, 0);
}

LONG load32(const volatile LONG* value) noexcept {
  return InterlockedCompareExchange(const_cast<volatile LONG*>(value), 0, 0);
}

bool mapping_region_is_valid(const void* view) noexcept {
  MEMORY_BASIC_INFORMATION information{};
  return view != nullptr &&
         VirtualQuery(view, &information, sizeof(information)) ==
             sizeof(information) &&
         information.AllocationBase == view &&
         information.State == MEM_COMMIT &&
         information.RegionSize >= kMappingBytes;
}

bool process_is_alive(std::uint32_t process_id) noexcept {
  if (process_id == 0) {
    return false;
  }
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
  if (process == nullptr) {
    return GetLastError() != ERROR_INVALID_PARAMETER;
  }
  const DWORD wait_result = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return wait_result == WAIT_TIMEOUT;
}

bool valid_header(const SharedHeader* value) noexcept {
  return value != nullptr && value->magic == kMagic &&
         value->version == kVersion &&
         value->header_bytes == kHeaderBytes &&
         value->mapping_bytes == kMappingBytes &&
         value->sample_rate > 0 &&
         value->channel_count == kChannelCount &&
         value->bytes_per_sample == sizeof(std::int32_t) &&
         value->capacity_blocks == kCapacityBlocks &&
         value->max_frames_per_block == kMaxFramesPerBlock &&
         value->frames_per_block > 0 &&
         value->frames_per_block <= kMaxFramesPerBlock &&
         value->format_checksum == detail::format_checksum(
                                       value->sample_rate,
                                       value->frames_per_block);
}

bool header_matches_format(
    const SharedHeader* value, const StreamFormat& format) noexcept {
  return valid_header(value) && value->sample_rate == format.sample_rate &&
         value->channel_count == format.channel_count &&
         value->frames_per_block == format.frames_per_block &&
         value->bytes_per_sample == format.bytes_per_sample;
}

}  // namespace

SharedRingPublisher::~SharedRingPublisher() {
  close();
}

bool SharedRingPublisher::open(
    std::uint32_t sample_rate,
    std::uint32_t frames_per_block,
    const wchar_t* mapping_name) noexcept {
  close();
  if (sample_rate == 0 || frames_per_block == 0 ||
      frames_per_block > kMaxFramesPerBlock || mapping_name == nullptr ||
      mapping_name[0] == L'\0') {
    return false;
  }

  mapping_ = CreateFileMappingW(
      INVALID_HANDLE_VALUE,
      nullptr,
      PAGE_READWRITE,
      0,
      static_cast<DWORD>(kMappingBytes),
      mapping_name);
  if (mapping_ == nullptr) {
    return false;
  }
  view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes);
  if (!mapping_region_is_valid(view_)) {
    if (view_ != nullptr) {
      UnmapViewOfFile(view_);
      view_ = nullptr;
    }
    CloseHandle(mapping_);
    mapping_ = nullptr;
    return false;
  }

  SharedHeader* shared = header(view_);
  if (shared->magic != kMagic || shared->version != kVersion ||
      shared->header_bytes != kHeaderBytes ||
      shared->mapping_bytes != kMappingBytes) {
    std::memset(shared, 0, kHeaderBytes);
    shared->magic = kMagic;
    shared->version = kVersion;
    shared->header_bytes = static_cast<std::uint32_t>(kHeaderBytes);
    shared->mapping_bytes = static_cast<std::uint32_t>(kMappingBytes);
  }

  if (InterlockedCompareExchange(&shared->producer_active, 1, 0) != 0) {
    const std::uint32_t owner = shared->producer_pid;
    if (owner == GetCurrentProcessId() || process_is_alive(owner)) {
      close();
      return false;
    }
    InterlockedCompareExchange(&shared->producer_active, 0, 1);
    if (InterlockedCompareExchange(&shared->producer_active, 1, 0) != 0) {
      close();
      return false;
    }
  }
  owns_producer_ = true;

  InterlockedExchange(&shared->ready, 0);
  const LONG64 previous_generation = load64(&shared->generation);
  const LONG64 configuring_generation =
      (previous_generation & 1) != 0 ? previous_generation
                                     : previous_generation + 1;
  InterlockedExchange64(&shared->generation, configuring_generation);
  format_ = {
      sample_rate,
      kChannelCount,
      frames_per_block,
      static_cast<std::uint32_t>(sizeof(std::int32_t))};
  shared->sample_rate = sample_rate;
  shared->channel_count = kChannelCount;
  shared->frames_per_block = frames_per_block;
  shared->bytes_per_sample = sizeof(std::int32_t);
  shared->capacity_blocks = kCapacityBlocks;
  shared->max_frames_per_block = kMaxFramesPerBlock;
  shared->format_checksum = detail::format_checksum(sample_rate, frames_per_block);
  shared->producer_pid = GetCurrentProcessId();
  InterlockedExchange64(&shared->write_sequence, 0);
  InterlockedExchange64(&shared->read_sequence, 0);
  InterlockedExchange64(&shared->dropped_blocks, 0);
  // File-backed pages are committed lazily. Touch every capture page before
  // streaming so the ASIO callback does not take first-touch page faults.
  std::memset(data(view_), 0, kDataBytes);
  InterlockedExchange64(&shared->generation, configuring_generation + 1);
  InterlockedExchange(&shared->ready, 1);
  return true;
}

void SharedRingPublisher::close() noexcept {
  if (view_ != nullptr && owns_producer_) {
    SharedHeader* shared = header(view_);
    InterlockedExchange(&shared->ready, 0);
    shared->producer_pid = 0;
    InterlockedExchange(&shared->producer_active, 0);
  }
  owns_producer_ = false;
  format_ = {};
  if (view_ != nullptr) {
    UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  if (mapping_ != nullptr) {
    CloseHandle(mapping_);
    mapping_ = nullptr;
  }
}

bool SharedRingPublisher::publish_int32_stereo(
    const std::int32_t* left,
    const std::int32_t* right) noexcept {
  if (view_ == nullptr || left == nullptr || right == nullptr ||
      format_.frames_per_block == 0) {
    return false;
  }
  SharedHeader* shared = header(view_);
  if (load32(&shared->ready) == 0 || !header_matches_format(shared, format_)) {
    return false;
  }

  const LONG64 write = load64(&shared->write_sequence);
  const LONG64 read = load64(&shared->read_sequence);
  if (write - read >= static_cast<LONG64>(kCapacityBlocks)) {
    InterlockedIncrement64(&shared->dropped_blocks);
    return false;
  }

  const std::size_t slot = static_cast<std::size_t>(write) % kCapacityBlocks;
  std::int32_t* destination = reinterpret_cast<std::int32_t*>(
      static_cast<std::byte*>(view_) + detail::block_offset(slot));
  const std::uint32_t frames = format_.frames_per_block;
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    destination[frame * 2] = left[frame];
    destination[frame * 2 + 1] = right[frame];
  }
  InterlockedExchange64(&shared->write_sequence, write + 1);
  return true;
}

bool SharedRingPublisher::is_open() const noexcept {
  return view_ != nullptr && owns_producer_;
}

StreamStats SharedRingPublisher::stats() const noexcept {
  if (view_ == nullptr) {
    return {};
  }
  const SharedHeader* shared = header(view_);
  return {
      static_cast<std::uint64_t>(load64(&shared->write_sequence)),
      static_cast<std::uint64_t>(load64(&shared->read_sequence)),
      static_cast<std::uint64_t>(load64(&shared->dropped_blocks))};
}

SharedRingReader::~SharedRingReader() {
  close();
}

bool SharedRingReader::open(
    const wchar_t* mapping_name, bool start_at_latest) noexcept {
  close();
  if (mapping_name == nullptr || mapping_name[0] == L'\0') {
    return false;
  }
  mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name);
  if (mapping_ == nullptr) {
    return false;
  }
  view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes);
  if (!mapping_region_is_valid(view_)) {
    if (view_ != nullptr) {
      UnmapViewOfFile(view_);
      view_ = nullptr;
    }
    CloseHandle(mapping_);
    mapping_ = nullptr;
    return false;
  }

  SharedHeader* shared = header(view_);
  const LONG64 generation = load64(&shared->generation);
  if ((generation & 1) != 0 || !valid_header(shared) ||
      load32(&shared->ready) == 0 || load32(&shared->producer_active) == 0) {
    close();
    return false;
  }
  if (InterlockedCompareExchange(&shared->consumer_active, 1, 0) != 0) {
    const std::uint32_t owner = shared->consumer_pid;
    if (owner == GetCurrentProcessId() || process_is_alive(owner)) {
      close();
      return false;
    }
    InterlockedCompareExchange(&shared->consumer_active, 0, 1);
    if (InterlockedCompareExchange(&shared->consumer_active, 1, 0) != 0) {
      close();
      return false;
    }
  }
  owns_consumer_ = true;
  format_ = {
      shared->sample_rate,
      shared->channel_count,
      shared->frames_per_block,
      shared->bytes_per_sample};
  if (load64(&shared->generation) != generation ||
      !header_matches_format(shared, format_)) {
    close();
    return false;
  }
  shared->consumer_pid = GetCurrentProcessId();
  if (start_at_latest) {
    InterlockedExchange64(
        &shared->read_sequence, load64(&shared->write_sequence));
  }
  return true;
}

void SharedRingReader::close() noexcept {
  if (view_ != nullptr && owns_consumer_) {
    SharedHeader* shared = header(view_);
    shared->consumer_pid = 0;
    InterlockedExchange(&shared->consumer_active, 0);
  }
  owns_consumer_ = false;
  format_ = {};
  if (view_ != nullptr) {
    UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  if (mapping_ != nullptr) {
    CloseHandle(mapping_);
    mapping_ = nullptr;
  }
}

bool SharedRingReader::read_int32_stereo(
    std::int32_t* interleaved,
    std::size_t sample_capacity,
    std::uint32_t* frames_read) noexcept {
  if (frames_read != nullptr) {
    *frames_read = 0;
  }
  if (view_ == nullptr || interleaved == nullptr || frames_read == nullptr) {
    return false;
  }

  SharedHeader* shared = header(view_);
  if (load32(&shared->ready) == 0 || !header_matches_format(shared, format_)) {
    return false;
  }
  const std::size_t required_samples =
      static_cast<std::size_t>(format_.frames_per_block) * kChannelCount;
  if (sample_capacity < required_samples) {
    return false;
  }

  const LONG64 read = load64(&shared->read_sequence);
  const LONG64 write = load64(&shared->write_sequence);
  if (read >= write) {
    return false;
  }

  const LONG64 generation = load64(&shared->generation);
  if ((generation & 1) != 0) {
    return false;
  }
  const std::size_t slot = static_cast<std::size_t>(read) % kCapacityBlocks;
  const std::int32_t* source = reinterpret_cast<const std::int32_t*>(
      static_cast<const std::byte*>(view_) + detail::block_offset(slot));
  std::memcpy(
      interleaved, source, required_samples * sizeof(std::int32_t));
  if (load64(&shared->generation) != generation ||
      load32(&shared->ready) == 0) {
    return false;
  }
  InterlockedExchange64(&shared->read_sequence, read + 1);
  *frames_read = format_.frames_per_block;
  return true;
}

bool SharedRingReader::is_open() const noexcept {
  return view_ != nullptr && owns_consumer_;
}

StreamFormat SharedRingReader::format() const noexcept {
  return format_;
}

StreamStats SharedRingReader::stats() const noexcept {
  if (view_ == nullptr) {
    return {};
  }
  const SharedHeader* shared = header(view_);
  return {
      static_cast<std::uint64_t>(load64(&shared->write_sequence)),
      static_cast<std::uint64_t>(load64(&shared->read_sequence)),
      static_cast<std::uint64_t>(load64(&shared->dropped_blocks))};
}

std::uint64_t SharedRingReader::queued_blocks() const noexcept {
  if (view_ == nullptr) {
    return 0;
  }
  const SharedHeader* shared = header(view_);
  if (load32(&shared->ready) == 0 || !header_matches_format(shared, format_)) {
    return 0;
  }
  const LONG64 read = load64(&shared->read_sequence);
  const LONG64 write = load64(&shared->write_sequence);
  if (write <= read) {
    return 0;
  }
  return (std::min)(
      static_cast<std::uint64_t>(write - read),
      static_cast<std::uint64_t>(kCapacityBlocks));
}

std::uint64_t SharedRingReader::discard_oldest(
    std::uint64_t keep_blocks) noexcept {
  if (view_ == nullptr) {
    return 0;
  }
  SharedHeader* shared = header(view_);
  if (load32(&shared->ready) == 0 || !header_matches_format(shared, format_)) {
    return 0;
  }
  keep_blocks = (std::min)(
      keep_blocks, static_cast<std::uint64_t>(kCapacityBlocks));
  const LONG64 read = load64(&shared->read_sequence);
  const LONG64 write = load64(&shared->write_sequence);
  if (write <= read + static_cast<LONG64>(keep_blocks)) {
    return 0;
  }
  const LONG64 replacement = write - static_cast<LONG64>(keep_blocks);
  InterlockedExchange64(&shared->read_sequence, replacement);
  return static_cast<std::uint64_t>(replacement - read);
}

SharedRingObserver::~SharedRingObserver() {
  close();
}

bool SharedRingObserver::open(const wchar_t* mapping_name) noexcept {
  close();
  if (mapping_name == nullptr || mapping_name[0] == L'\0') {
    return false;
  }
  mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name);
  if (mapping_ == nullptr) {
    return false;
  }
  view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, kMappingBytes);
  if (!mapping_region_is_valid(view_)) {
    if (view_ != nullptr) {
      UnmapViewOfFile(view_);
      view_ = nullptr;
    }
    CloseHandle(mapping_);
    mapping_ = nullptr;
    return false;
  }
  if (!valid_header(header(view_))) {
    close();
    return false;
  }
  return true;
}

void SharedRingObserver::close() noexcept {
  if (view_ != nullptr) {
    UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  if (mapping_ != nullptr) {
    CloseHandle(mapping_);
    mapping_ = nullptr;
  }
}

bool SharedRingObserver::snapshot(StreamStatus* status) const noexcept {
  if (status == nullptr) {
    return false;
  }
  *status = {};
  if (view_ == nullptr) {
    return false;
  }
  const SharedHeader* shared = header(view_);
  if (!valid_header(shared)) {
    return false;
  }
  // The observer maps the section read-only. Its values are informational, so
  // aligned volatile loads are sufficient and avoid write-style interlocked
  // operations on a read-only view.
  const bool producer_alive =
      shared->producer_active != 0 && process_is_alive(shared->producer_pid);
  const bool consumer_alive =
      shared->consumer_active != 0 && process_is_alive(shared->consumer_pid);
  status->producer_active = shared->ready != 0 && producer_alive;
  status->consumer_active = consumer_alive;
  status->producer_pid = producer_alive ? shared->producer_pid : 0;
  status->consumer_pid = consumer_alive ? shared->consumer_pid : 0;
  status->format = {
      shared->sample_rate,
      shared->channel_count,
      shared->frames_per_block,
      shared->bytes_per_sample};
  // These counters are diagnostic snapshots and need not be one transaction.
  status->stats = {
      static_cast<std::uint64_t>(shared->write_sequence),
      static_cast<std::uint64_t>(shared->read_sequence),
      static_cast<std::uint64_t>(shared->dropped_blocks)};
  return true;
}

bool SharedRingObserver::is_open() const noexcept {
  return view_ != nullptr;
}

}  // namespace soundfork::capture
