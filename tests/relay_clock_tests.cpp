#include <windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "capture-backend/relay_clock.hpp"
#include "capture-backend/endpoint_selection.hpp"
#include "capture-backend/relay_recovery.hpp"
#include "capture-backend/shared_ring.hpp"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_four_hour_drift(double producer_ppm) {
  constexpr std::uint32_t kSampleRate = 48000;
  constexpr std::uint32_t kBlockFrames = 64;
  constexpr std::uint32_t kEventFrames = 480;
  constexpr std::uint64_t kEventCount = 4ULL * 60 * 60 * 100;
  soundfork::capture::RelayClockController controller(
      kSampleRate, kBlockFrames);
  double buffered = static_cast<double>(controller.target_frames());
  double minimum = buffered;
  double maximum = buffered;
  for (std::uint64_t event = 0; event < kEventCount; ++event) {
    const double ratio = controller.update(
        static_cast<std::uint64_t>(std::llround(buffered)), kEventFrames);
    buffered += kEventFrames * (1.0 + producer_ppm * 0.000001) -
                kEventFrames * ratio;
    minimum = (std::min)(minimum, buffered);
    maximum = (std::max)(maximum, buffered);
    require(buffered > 2.0, "drift controller reached underflow");
    require(
        buffered < soundfork::capture::kCapacityBlocks * kBlockFrames,
        "drift controller reached ring capacity");
  }
  require(maximum - minimum < 1024.0, "relay queue was not bounded");
  require(
      std::abs(controller.correction_ppm() - producer_ppm) < 20.0,
      "clock correction did not converge");
}

void test_linear_relay_source() {
  const std::wstring mapping_name =
      L"Local\\SoundFork.RelayClockTest." +
      std::to_wstring(GetCurrentProcessId());
  soundfork::capture::SharedRingPublisher publisher;
  require(publisher.open(48000, 64, mapping_name.c_str()), "publisher failed");
  soundfork::capture::SharedRingReader reader;
  require(reader.open(mapping_name.c_str(), false), "reader failed");

  std::array<std::int32_t, 64> left{};
  std::array<std::int32_t, 64> right{};
  for (std::uint32_t block = 0; block < 4; ++block) {
    for (std::uint32_t frame = 0; frame < 64; ++frame) {
      const std::int32_t value = static_cast<std::int32_t>(block * 64 + frame);
      left[frame] = value;
      right[frame] = -value;
    }
    require(
        publisher.publish_int32_stereo(left.data(), right.data()),
        "publish failed");
  }

  auto source =
      std::make_unique<soundfork::capture::RelayAudioSource>(reader.format());
  std::array<std::int32_t, 256> output{};
  require(source->render(reader, output.data(), 128, 1.0), "render failed");
  for (std::int32_t frame = 0; frame < 128; ++frame) {
    require(output[frame * 2] == frame, "left resample mismatch");
    require(output[frame * 2 + 1] == -frame, "right resample mismatch");
  }
  require(source->metrics().silent_frames == 0, "unexpected relay underflow");
}

void test_reconnect_error_policy() {
  using soundfork::capture::endpoint_matches;
  require(endpoint_matches(L"opaque-A", L"CABLE Input", L"opaque-A", true), "saved ID not selected");
  require(!endpoint_matches(L"opaque-B", L"CABLE Input", L"opaque-A", true), "same-name replacement selected");
  require(!endpoint_matches(L"opaque-A", L"CABLE Input", L"CABLE", true), "saved ID treated as a name");
  require(!endpoint_matches(L"opaque-A", L"CABLE Input", L"", true), "empty ID selected a device");
  require(!endpoint_matches(L"opaque-A", L"CABLE Input", L"opaque", true), "partial ID selected a device");
  require(endpoint_matches(L"opaque-A", L"CABLE Input", L"cable", false), "legacy name search failed");
  require(
      soundfork::capture::recoverable_audio_error(
          AUDCLNT_E_DEVICE_INVALIDATED),
      "device invalidation was not recoverable");
  require(
      soundfork::capture::recoverable_audio_error(
          AUDCLNT_E_SERVICE_NOT_RUNNING),
      "audio service restart was not recoverable");
  require(
      !soundfork::capture::recoverable_audio_error(E_INVALIDARG),
      "programming error was incorrectly recoverable");
}

}  // namespace

int main() {
  try {
    test_four_hour_drift(1500.0);
    test_four_hour_drift(-1500.0);
    test_linear_relay_source();
    test_reconnect_error_policy();
    std::cout << "SoundFork relay clock tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "SoundFork relay clock test failure: " << error.what() << '\n';
    return 1;
  }
}
