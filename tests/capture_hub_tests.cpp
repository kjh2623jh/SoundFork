#include <windows.h>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "capture-backend/hub_client.hpp"
#include "capture-backend/shared_ring.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class Predicate>
void await(Predicate predicate, const char* message) {
  const auto deadline = GetTickCount64() + 5000;
  while (!predicate()) {
    require(GetTickCount64() < deadline, message);
    Sleep(2);
  }
}

void test_raw_formats() {
  using namespace soundfork::capture;
  const auto scope = L"format-" + std::to_wstring(GetCurrentProcessId());
  const HubNames names(scope);
  std::uint32_t hub_pid = 0;
  {
    // Keep one hub alive across format changes. Repeating its two-second idle
    // exit per format adds cost, not PCM coverage; the lifecycle test remains.
    HubClient relay_client(HubRole::relay, scope), obs_client(HubRole::obs, scope);
    for (auto format : {SampleFormat::s16le, SampleFormat::s24le, SampleFormat::s32le, SampleFormat::f32le}) {
      std::cout << "Hub raw format: " << sample_format_name(format) << std::endl;
      SharedRingPublisher producer;
      require(producer.open(48000, 64, names.input.c_str(), format), "raw input failed");
      SharedRingReader relay, obs;
      await([&] { relay_client.ensure_running(); return relay.is_open() || relay.open(relay_client.mapping_name()); }, "raw relay attach");
      await([&] { return obs.open(obs_client.mapping_name()); }, "raw OBS attach");
      require(relay.format().sample_format == format && obs.format().sample_format == format, "hub erased sample format");
      SharedRingObserver observer; StreamStatus status;
      require(observer.open(names.input.c_str()) && observer.snapshot(&status) && status.consumer_active, "hub owner missing");
      if (hub_pid == 0) hub_pid = status.consumer_pid;
      require(status.consumer_pid == hub_pid, "format changes unexpectedly replaced the hub process");
      std::array<std::byte, 256> left{}, right{};
      std::array<std::byte, 512> pcm{};
      const auto plane = static_cast<std::size_t>(64 * sample_bytes(format));
      for (std::size_t i = 0; i < plane; ++i) { left[i] = static_cast<std::byte>(i & 255); right[i] = static_cast<std::byte>((255 - i) & 255); }
      std::uint32_t frames = 0;
      require(producer.publish_planar(left.data(), right.data()), "raw input publication");
      for (auto* reader : {&relay, &obs}) {
        await([&] { return reader->read_planar(pcm.data(), pcm.size(), &frames); }, "raw fanout missing");
        require(frames == 64 && std::memcmp(pcm.data(), left.data(), plane) == 0 &&
                std::memcmp(pcm.data() + plane, right.data(), plane) == 0, "hub transformed raw payload");
      }
      producer.close();
      await([&] { return !relay.stream_active() && !obs.stream_active(); }, "raw source stop propagation");
    }
  }
  await([&] { HANDLE ready = OpenEventW(SYNCHRONIZE, FALSE, names.ready.c_str());
    if (!ready) return true; CloseHandle(ready); return false; }, "raw hub exit");
}
}  // namespace

int main(int argc, char** argv) {
  using namespace soundfork::capture;
  try {
    if (argc == 2 && std::string(argv[1]) == "--formats") {
      test_raw_formats(); std::cout << "All four PCM formats fan out byte-for-byte\n"; return 0;
    }
    const auto scope = L"test-" + std::to_wstring(GetCurrentProcessId());
    const HubNames names(scope);
    SharedRingPublisher producer;
    require(producer.open(48000, 64, names.input.c_str()), "input open failed");
    {
      HubClient relay_client(HubRole::relay, scope);
      HubClient obs_client(HubRole::obs, scope);
      SharedRingReader relay;
      SharedRingReader obs;
      await([&] {
        relay_client.ensure_running();
        return relay.is_open() || relay.open(relay_client.mapping_name());
      }, "relay could not connect to hub");
      await([&] { return obs.open(obs_client.mapping_name()); }, "OBS could not connect to hub");
      // Repeated ensure must reuse the same hub, never another ASIO consumer.
      for (int index = 0; index < 3; ++index) require(obs_client.ensure_running(), "hub not ready");
      std::array<std::int32_t, 64> left{};
      std::array<std::int32_t, 64> right{};
      std::array<std::int32_t, 128> pcm{};
      std::uint32_t frames = 0;
      for (int block = 1; block <= 80; ++block) {
        left.fill(block * 1000);
        right.fill(-block * 1000);
        require(producer.publish_int32_stereo(left.data(), right.data()), "input dropped unexpectedly");
        await([&] { return relay.read_int32_stereo(pcm.data(), pcm.size(), &frames); },
              "slow OBS blocked relay");
        require(frames == 64, "wrong frame count");
        for (std::size_t frame = 0; frame < 64; ++frame) {
          require(pcm[frame * 2] == left[frame] && pcm[frame * 2 + 1] == right[frame],
                  "fan-out changed PCM or channel order");
        }
        if (block == 1) {
          await([&] { return obs.read_int32_stereo(pcm.data(), pcm.size(), &frames); }, "OBS PCM missing");
          require(pcm[0] == 1000 && pcm[1] == -1000, "OBS PCM corrupted");
        }
      }
      require(obs.stats().dropped_blocks > 0, "stalled OBS did not fill its private queue");
      require(relay.stats().dropped_blocks == 0, "stalled OBS affected relay");
      require(producer.stats().dropped_blocks == 0, "hub blocked input producer");
      producer.close();
      await([&] { return !relay.stream_active() && !obs.stream_active(); }, "producer stop was not propagated");
      relay.close();
      obs.close();
      require(producer.open(48000, 64, names.input.c_str()), "same-format producer restart failed");
      await([&] { return relay.open(relay_client.mapping_name()); }, "relay did not reconnect");
      await([&] { return obs.open(obs_client.mapping_name()); }, "OBS did not reconnect");
      left.fill(123456);
      right.fill(-654321);
      require(producer.publish_int32_stereo(left.data(), right.data()), "restart publish failed");
      await([&] { return obs.read_int32_stereo(pcm.data(), pcm.size(), &frames); }, "restart PCM missing");
      require(pcm[0] == 123456 && pcm[1] == -654321, "stale PCM after restart");

      // Kill only the hub that owns this test's private input mapping. Real
      // user mappings/processes are never inspected or terminated by this test.
      SharedRingObserver observer;
      StreamStatus status;
      require(observer.open(names.input.c_str()) && observer.snapshot(&status), "test hub PID unavailable");
      HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, status.consumer_pid);
      if (process == nullptr) throw std::runtime_error("cannot open test hub process");
      const bool killed = TerminateProcess(process, 99) != FALSE;
      const DWORD exited = WaitForSingleObject(process, 2000);
      CloseHandle(process);
      require(killed && exited == WAIT_OBJECT_0, "test hub termination failed");
      require(!relay.stream_active() && !obs.stream_active(), "hub death not detected");
      relay.close();
      obs.close();
      await([&] {
        relay_client.ensure_running();
        return relay.open(relay_client.mapping_name());
      }, "hub did not recover after crash");
      await([&] { return obs.open(obs_client.mapping_name()); }, "OBS did not recover after hub crash");
      require(producer.publish_int32_stereo(left.data(), right.data()), "post-crash publish failed");
      await([&] { return obs.read_int32_stereo(pcm.data(), pcm.size(), &frames); }, "post-crash PCM missing");
      require(pcm[0] == 123456 && pcm[1] == -654321, "post-crash PCM mismatch");
    }
    await([&] {
      HANDLE ready = OpenEventW(SYNCHRONIZE, FALSE, names.ready.c_str());
      if (ready == nullptr) return true;
      CloseHandle(ready);
      return false;
    }, "hub did not exit after all clients closed");
    std::cout << "Capture hub: simultaneous PCM, slow-consumer isolation, restart, crash recovery, auto-exit passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
