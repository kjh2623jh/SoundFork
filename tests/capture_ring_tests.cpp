#include <windows.h>

#include <array>
#include <cstdint>
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

  publisher.close();
  require(!publisher.is_open(), "publisher close failed");
}

}  // namespace

int main() {
  try {
    test_shared_ring();
    std::cout << "SoundFork shared capture ring tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "SoundFork shared capture ring test failure: " << error.what()
              << '\n';
    return 1;
  }
}
