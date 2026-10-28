#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "capture-backend/shared_ring.hpp"
#include "capture-backend/shared_ring_layout.hpp"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_shared_ring() {
  const std::wstring mapping_name =
      L"Local\\SoundFork.CaptureTest." + std::to_wstring(GetCurrentProcessId());
  soundfork::capture::SharedRingPublisher publisher;
  require(
      publisher.open(48000, 64, mapping_name.c_str()),
      "publisher open failed");

  soundfork::capture::SharedRingPublisher second_publisher;
  require(
      !second_publisher.open(48000, 64, mapping_name.c_str()),
      "second producer was not rejected");
  require(publisher.is_open(), "second producer disrupted the first producer");

  soundfork::capture::SharedRingObserver observer;
  require(observer.open(mapping_name.c_str()), "observer open failed");
  soundfork::capture::StreamStatus initial_status;
  require(observer.snapshot(&initial_status), "observer snapshot failed");
  require(initial_status.producer_active, "observer missed producer");
  require(!initial_status.consumer_active, "observer claimed consumer slot");
  require(
      initial_status.format.sample_rate == 48000 &&
          initial_status.format.frames_per_block == 64,
      "observer format mismatch");

  soundfork::capture::SharedRingReader reader;
  require(reader.open(mapping_name.c_str()), "reader open failed");
  soundfork::capture::SharedRingReader second_reader;
  require(
      !second_reader.open(mapping_name.c_str()),
      "second consumer was not rejected");
  require(reader.is_open(), "second consumer disrupted the first consumer");
  soundfork::capture::StreamStatus reader_status;
  require(observer.snapshot(&reader_status), "reader snapshot failed");
  require(reader_status.consumer_active, "observer missed consumer");
  const auto format = reader.format();
  require(
      format.sample_rate == 48000 && format.channel_count == 2 &&
          format.frames_per_block == 64 && format.bytes_per_sample == 4,
      "unexpected stream format");

  std::array<std::int32_t, 64> left{};
  std::array<std::int32_t, 64> right{};
  for (std::size_t frame = 0; frame < left.size(); ++frame) {
    left[frame] = static_cast<std::int32_t>(frame);
    right[frame] = static_cast<std::int32_t>(1000 + frame);
  }
  require(
      publisher.publish_int32_stereo(left.data(), right.data()),
      "initial publish failed");

  std::array<std::int32_t, 128> output{};
  std::uint32_t frames_read = 0;
  require(
      reader.read_int32_stereo(output.data(), output.size(), &frames_read),
      "initial read failed");
  require(frames_read == 64, "read frame count mismatch");
  for (std::size_t frame = 0; frame < left.size(); ++frame) {
    require(output[frame * 2] == left[frame], "left sample mismatch");
    require(output[frame * 2 + 1] == right[frame], "right sample mismatch");
  }

  HANDLE mapping = OpenFileMappingW(
      FILE_MAP_ALL_ACCESS, FALSE, mapping_name.c_str());
  if (mapping == nullptr) {
    throw std::runtime_error("test could not reopen shared mapping");
  }
  void* view = MapViewOfFile(
      mapping,
      FILE_MAP_ALL_ACCESS,
      0,
      0,
      soundfork::capture::detail::kMappingBytes);
  if (view == nullptr) {
    CloseHandle(mapping);
    throw std::runtime_error("test could not map shared layout");
  }
  auto* shared = static_cast<soundfork::capture::detail::SharedHeader*>(view);
  const std::uint32_t original_frames = shared->frames_per_block;
  shared->frames_per_block = soundfork::capture::kMaxFramesPerBlock;
  require(
      !publisher.publish_int32_stereo(left.data(), right.data()),
      "publisher trusted a modified shared format");
  std::uint32_t rejected_frames = 123;
  require(
      !reader.read_int32_stereo(
          output.data(), output.size(), &rejected_frames) &&
          rejected_frames == 0,
      "reader trusted a modified shared format");
  shared->frames_per_block = original_frames;
  UnmapViewOfFile(view);
  CloseHandle(mapping);

  for (std::uint32_t block = 0;
       block < soundfork::capture::kCapacityBlocks;
       ++block) {
    require(
        publisher.publish_int32_stereo(left.data(), right.data()),
        "ring became full too early");
  }
  require(
      !publisher.publish_int32_stereo(left.data(), right.data()),
      "full ring did not drop the block");
  require(publisher.stats().dropped_blocks == 1, "drop counter mismatch");
  require(
      reader.queued_blocks() == soundfork::capture::kCapacityBlocks,
      "queued block count mismatch");
  require(reader.discard_oldest(4) == 60, "stale block discard mismatch");
  require(reader.queued_blocks() == 4, "discard did not retain four blocks");
  require(
      publisher.publish_int32_stereo(left.data(), right.data()),
      "publisher did not recover after relay discard");

  publisher.close();
  require(!publisher.is_open(), "publisher close failed");
}

void test_raw_layout_and_validation() {
  using namespace soundfork::capture;
  const auto name = L"Local\\SoundFork.RawRingTest." + std::to_wstring(GetCurrentProcessId());
  SharedRingPublisher publisher;
  SharedRingReader reader;
  for (auto format : {SampleFormat::s16le, SampleFormat::s24le, SampleFormat::s32le, SampleFormat::f32le}) {
    require(publisher.open(48000, 64, name.c_str(), format), "raw publisher");
    require(reader.open(name.c_str(), false), "raw reader");
    const auto description = reader.format();
    require(description.sample_format == format && description.layout == ChannelLayout::planar &&
            description.payload_bytes == 128 * sample_bytes(format), "wire format mismatch");
    std::array<std::byte, 256> left{}, right{};
    std::array<std::byte, 512> raw{};
    const auto plane = description.payload_bytes / 2;
    for (std::size_t i = 0; i < plane; ++i) { left[i] = static_cast<std::byte>(i & 255); right[i] = static_cast<std::byte>((i + 1) & 255); }
    require(publisher.publish_planar(left.data(), right.data()), "raw publish");
    std::uint32_t frames = 99;
    require(!reader.read_planar(raw.data(), description.payload_bytes - 1, &frames) && frames == 0, "short raw destination");
    require(reader.read_planar(raw.data(), raw.size(), &frames) && frames == 64, "raw read");
    require(std::memcmp(raw.data(), left.data(), plane) == 0 &&
            std::memcmp(raw.data() + plane, right.data(), plane) == 0, "planar byte identity");
    struct Mapping {
      HANDLE handle = nullptr; void* view = nullptr;
      ~Mapping() { if (view) UnmapViewOfFile(view); if (handle) CloseHandle(handle); }
    } mapping;
    mapping.handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    require(mapping.handle != nullptr, "raw map handle");
    mapping.view = MapViewOfFile(mapping.handle, FILE_MAP_ALL_ACCESS, 0, 0, detail::kMappingBytes);
    require(mapping.view != nullptr, "raw map view");
    auto* header = static_cast<detail::SharedHeader*>(mapping.view);
    const auto corrupt = [&](std::uint32_t* field, std::uint32_t invalid) {
      const auto saved = *field; *field = invalid;
      require(!publisher.publish_planar(left.data(), right.data()), "modified header accepted by publisher");
      SharedRingReader other;
      require(!other.open(name.c_str()), "modified header accepted by new reader");
      require(!reader.read_planar(raw.data(), raw.size(), &frames), "modified header accepted by reader");
      *field = saved;
    };
    corrupt(&header->version, 2);
    corrupt(&header->sample_format, 999);
    corrupt(&header->channel_layout, 2);
    corrupt(&header->bytes_per_sample, UINT32_MAX);
    corrupt(&header->frames_per_block, UINT32_MAX);
    corrupt(&header->payload_bytes, UINT32_MAX);
    const auto write = header->write_sequence;
    InterlockedExchange64(&header->write_sequence, (std::numeric_limits<LONG64>::max)());
    require(!publisher.publish_planar(left.data(), right.data()) &&
            !reader.read_planar(raw.data(), raw.size(), &frames) && reader.discard_oldest(0) == 0,
            "corrupt sequences accepted");
    InterlockedExchange64(&header->write_sequence, write);
    publisher.close();
    require(publisher.open(48000, 64, name.c_str(), format), "raw generation restart");
    require(!reader.stream_active() && !reader.read_planar(raw.data(), raw.size(), &frames), "stale generation readable");
    reader.close(); publisher.close();
  }
  require(!publisher.open(48000, UINT32_MAX, name.c_str()) &&
          !publisher.open(48000, 64, name.c_str(), SampleFormat::invalid), "invalid publisher format accepted");
  require(publisher.open_disabled(CaptureError::sample_format, name.c_str()), "disabled diagnostic mapping");
  SharedRingObserver observer; StreamStatus status;
  require(observer.open(name.c_str()) && observer.snapshot(&status) && !status.producer_active &&
          status.capture_error == CaptureError::sample_format && !reader.open(name.c_str()), "disabled mapping consumable");
}

}  // namespace

int main() {
  try {
    test_shared_ring();
    test_raw_layout_and_validation();
    std::cout << "SoundFork shared capture ring tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "SoundFork shared capture ring test failure: " << error.what()
              << '\n';
    return 1;
  }
}
