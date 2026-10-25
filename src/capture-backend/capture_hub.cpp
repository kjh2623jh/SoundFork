#include <windows.h>

#include <array>
#include <cwctype>
#include <memory>
#include <string>

#include "capture-backend/hub_client.hpp"
#include "capture-backend/shared_ring.hpp"
#include "diagnostics/async_log.hpp"
#include "soundfork_version.hpp"

namespace {
struct Handle {
  HANDLE value = nullptr;
  ~Handle() { if (value != nullptr) CloseHandle(value); }
};

struct Buffers {
  std::array<std::int32_t, soundfork::capture::kMaxFramesPerBlock * 2> input{};
  std::array<std::int32_t, soundfork::capture::kMaxFramesPerBlock> left{};
  std::array<std::int32_t, soundfork::capture::kMaxFramesPerBlock> right{};
};
}  // namespace

int hub_main(int argc, wchar_t** argv) {
  using namespace soundfork::capture;
  std::wstring scope;
  if (argc == 3 && std::wstring(argv[1]) == L"--scope") {
    scope = argv[2];
    if (scope.empty() || scope.size() > 64) return 2;
    for (const auto character : scope) {
      if (!std::iswalnum(character) && character != L'-') return 2;
    }
  } else if (argc != 1) {
    return 2;
  }
  const HubNames names(scope);
  Handle singleton{CreateMutexW(nullptr, TRUE, names.singleton.c_str())};
  if (singleton.value == nullptr) return 3;
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  // Isolated tests must not write the installed user's diagnostic logs.
  if (scope.empty()) soundfork::diagnostics::initialize_log(
      soundfork::diagnostics::Component::hub, SOUNDFORK_VERSION_STRING);
  auto& logger = soundfork::diagnostics::log();
  logger.write(soundfork::diagnostics::Level::info, "hub_start");
  Handle ready{CreateEventW(nullptr, TRUE, FALSE, names.ready.c_str())};
  Handle timer{create_periodic_timer(50)};
  if (ready.value == nullptr || timer.value == nullptr) return 3;

  auto buffers = std::make_unique<Buffers>();
  SharedRingReader input;
  SharedRingPublisher relay;
  SharedRingPublisher obs;
  ULONGLONG check_clients_at = 0;
  ULONGLONG no_clients_since = 0;
  ULONGLONG retry_at = 0;
  ULONGLONG check_stream_at = 0;
  ULONGLONG next_metrics_at = GetTickCount64() + 30000;
  SetEvent(ready.value);
  while (WaitForSingleObject(timer.value, 1000) == WAIT_OBJECT_0) {
    const auto now = GetTickCount64();
    if (now >= check_clients_at) {
      check_clients_at = now + 250;
      if (hub_has_clients(names)) {
        no_clients_since = 0;
      } else if (no_clients_since == 0) {
        no_clients_since = now;
      } else if (now - no_clients_since >= 2000) {
        break;
      }
    }
    if (now >= check_stream_at) {
      check_stream_at = now + 50;
      if (input.is_open() && !input.stream_active()) {
        logger.write(soundfork::diagnostics::Level::info, "hub_source_disconnected");
        input.close();
        relay.close();
        obs.close();
        if (!set_timer_period(timer.value, 50)) break;
      }
    }
    if (!input.is_open()) {
      if (now < retry_at) continue;
      retry_at = now + 50;
      if (!input.open(names.input.c_str())) continue;
      const auto format = input.format();
      if (!relay.open(format.sample_rate, format.frames_per_block,
                      names.relay.c_str()) ||
          !obs.open(format.sample_rate, format.frames_per_block,
                    names.obs.c_str())) {
        input.close();
        relay.close();
        obs.close();
        continue;
      }
      if (!set_timer_period(timer.value, 1)) break;
      logger.write(soundfork::diagnostics::Level::info, "hub_source_connected",
          "rate=" + std::to_string(format.sample_rate) + " frames_per_block=" + std::to_string(format.frames_per_block));
    }
    // Bound work per wake. A stalled consumer only fills its own output ring.
    for (std::uint32_t block = 0; block < kCapacityBlocks; ++block) {
      std::uint32_t frames = 0;
      if (!input.read_int32_stereo(buffers->input.data(),
                                   buffers->input.size(), &frames)) break;
      for (std::uint32_t frame = 0; frame < frames; ++frame) {
        buffers->left[frame] = buffers->input[frame * 2];
        buffers->right[frame] = buffers->input[frame * 2 + 1];
      }
      if (relay.consumer_active()) relay.publish_int32_stereo(buffers->left.data(), buffers->right.data());
      if (obs.consumer_active()) obs.publish_int32_stereo(buffers->left.data(), buffers->right.data());
    }
    if (now >= next_metrics_at) {
      next_metrics_at = now + 30000;
      const auto stats = input.stats();
      logger.write(soundfork::diagnostics::Level::info, "hub_metrics",
          "asio_published=" + std::to_string(stats.published_blocks) +
          " asio_drops=" + std::to_string(stats.dropped_blocks) +
          " relay_drops=" + std::to_string(relay.stats().dropped_blocks) +
          " obs_drops=" + std::to_string(obs.stats().dropped_blocks));
    }
  }
  ResetEvent(ready.value);
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  const int result = hub_main(argc, argv);
  auto& logger = soundfork::diagnostics::log();
  logger.write(result == 0 ? soundfork::diagnostics::Level::info : soundfork::diagnostics::Level::error,
      "hub_exit", "exit_code=" + std::to_string(result));
  logger.stop();
  return result;
}
