#include <windows.h>
#include <obs.h>

#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "capture-backend/hub_client.hpp"
#include "capture-backend/shared_ring.hpp"

namespace {
struct Observed {
  std::atomic<std::uint64_t> packets{0};
  std::atomic<bool> stereo_pcm{false};
  std::atomic<bool> timestamps_bad{false};
  std::uint64_t last_timestamp = 0;
};
void capture(void* data, obs_source_t*, const audio_data* audio, bool) {
  auto& seen = *static_cast<Observed*>(data);
  if (audio->timestamp <= seen.last_timestamp) seen.timestamps_bad.store(true);
  seen.last_timestamp = audio->timestamp;
  const auto* left = reinterpret_cast<const float*>(audio->data[0]);
  const auto* right = reinterpret_cast<const float*>(audio->data[1]);
  if (left != nullptr && right != nullptr) {
    for (std::uint32_t frame = 0; frame < audio->frames; ++frame) {
      if (std::abs(left[frame] - 0.25F) < 0.001F &&
          std::abs(right[frame] + 0.125F) < 0.001F) seen.stereo_pcm.store(true);
    }
  }
  ++seen.packets;
}
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main(int argc, char** argv) {
  using namespace soundfork::capture;
  if (argc != 3) return 2;
  const auto scope = L"obs-test-" + std::to_wstring(GetCurrentProcessId());
  SetEnvironmentVariableW(L"SOUNDFORK_CAPTURE_SCOPE", scope.c_str());
  const HubNames names(scope);
  bool started = false;
  obs_source_t* source = nullptr;
  Observed seen;
  try {
    require(obs_startup("en-US", nullptr, nullptr), "OBS startup failed");
    started = true;
    obs_audio_info info{};
    info.samples_per_sec = 48000;
    info.speakers = SPEAKERS_STEREO;
    require(obs_reset_audio(&info), "OBS audio init failed");
    obs_module_t* module = nullptr;
    require(obs_open_module(&module, argv[1], argv[2]) == MODULE_SUCCESS, "plugin DLL load failed");
    require(obs_init_module(module), "plugin registration failed");
    source = obs_source_create("soundfork_audio", "Test SoundFork", nullptr, nullptr);
    require(source != nullptr, "source create failed");
    obs_source_add_audio_capture_callback(source, capture, &seen);
    // Source starts before the game and without WASAPI or a virtual device.
    Sleep(100);
    SharedRingPublisher publisher;
    require(publisher.open(48000, 64, names.input.c_str()), "test publisher failed");
    HubClient relay_client(HubRole::relay, scope);
    SharedRingReader relay;
    std::array<std::int32_t, 64> left{};
    std::array<std::int32_t, 64> right{};
    std::array<std::int32_t, 128> output{};
    left.fill(536870912);
    right.fill(-268435456);
    HANDLE timer = create_periodic_timer(4);
    if (timer == nullptr) throw std::runtime_error("test timer failed");
    struct TimerCloser { HANDLE value; ~TimerCloser() { CloseHandle(value); } } closer{timer};
    const auto deadline = GetTickCount64() + 2500;
    std::uint64_t relay_blocks = 0;
    while (GetTickCount64() < deadline) {
      WaitForSingleObject(timer, 1000);
      // Three 64-frame blocks every 4 ms = 48 kHz.
      for (int block = 0; block < 3; ++block) publisher.publish_int32_stereo(left.data(), right.data());
      if (!relay.is_open()) relay.open(relay_client.mapping_name());
      std::uint32_t frames = 0;
      while (relay.read_int32_stereo(output.data(), output.size(), &frames)) ++relay_blocks;
    }
    require(seen.packets.load() > 10, "OBS did not receive audio packets");
    require(seen.stereo_pcm.load(), "OBS did not receive the expected stereo PCM");
    require(!seen.timestamps_bad.load(), "OBS timestamps went backwards");
    require(relay_blocks > 100, "simultaneous relay capture failed");
    obs_source_remove_audio_capture_callback(source, capture, &seen);
    obs_source_release(source);
    source = nullptr;
    obs_shutdown();
    started = false;
    std::cout << "OBS plugin loaded by libobs: stereo PCM, timestamps, concurrent relay passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    if (source != nullptr) {
      obs_source_remove_audio_capture_callback(source, capture, &seen);
      obs_source_release(source);
    }
    if (started) obs_shutdown();
    return 1;
  }
}
