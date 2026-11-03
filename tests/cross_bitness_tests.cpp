#include <windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include "audio-bridge/proxy_driver.hpp"
#include "capture-backend/hub_client.hpp"
#include "capture-backend/shared_ring_layout.hpp"
#include "fake_asio_driver.hpp"
#include "pcm_fixture.hpp"

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> void await(Predicate predicate) {
  const auto end = GetTickCount64() + 15000;
  while (!predicate()) { require(GetTickCount64() < end, "interop deadline exceeded"); Sleep(1); }
}
ASIOBufferInfo* buffers = nullptr;
std::size_t plane = 0;
int sequence = 0;
std::array<std::byte, 2048> expected{};
void prepare(int index) {
  for (std::size_t i = 0; i < plane; ++i) {
    expected[i] = static_cast<std::byte>((i + static_cast<unsigned>(index) * 7) & 255);
    expected[plane + i] = static_cast<std::byte>((255 - i - static_cast<unsigned>(index) * 7) & 255);
  }
}
void callback(long index, ASIOBool) {
  prepare(sequence++);
  std::memcpy(buffers[0].buffers[index], expected.data(), plane);
  std::memcpy(buffers[1].buffers[index], expected.data() + plane, plane);
}
void rate(ASIOSampleRate) {}
long message(long, long, void*, double*) { return 0; }
ASIOTime* timed(ASIOTime* time, long index, ASIOBool direct) { callback(index, direct); return time; }
struct Handle { HANDLE value; ~Handle() { if(value) CloseHandle(value); } };
}
int wmain(int argc, wchar_t** argv) {
  using namespace soundfork::capture;
  if (argc != 5) return 2;
  const bool producer = std::wstring_view(argv[1]) == L"producer";
  const std::wstring scope(argv[2]);
  const int format_index = _wtoi(argv[3]);
  const long frames = _wtol(argv[4]);
  if (format_index < 0 || format_index >= 4 || (frames != 64 && frames != 256)) return 2;
  const auto& format = soundfork::test::kPcmCases[format_index];
  plane = static_cast<std::size_t>(frames) * format.bytes;
  const HubNames names(scope);
  const auto ack_name = L"Local\\SoundFork.Interop.Ack." + scope;
  try {
    Handle ack{CreateEventW(nullptr, FALSE, FALSE, ack_name.c_str())};
    require(ack.value != nullptr, "ack create");
    if (producer) {
      soundfork::test::FakeBackendFactory factory({frames});
      auto* proxy = new soundfork::ProxyDriver(factory, names.input.c_str());
      require(proxy->init(nullptr) == ASIOTrue, "proxy init");
      auto* fake = factory.last_created();
      fake->set_channel_type(0, format.asio); fake->set_channel_type(1, format.asio);
      ASIOBufferInfo output[2]{}; output[1].channelNum = 1; buffers = output;
      ASIOCallbacks callbacks{callback, rate, message, timed};
      require(proxy->createBuffers(output, 2, frames, &callbacks) == ASE_OK, "proxy buffers");
      require(proxy->start() == ASE_OK, "proxy start");
      // Waits belong only to this fake-host harness, never the ASIO callback.
      require(WaitForSingleObject(ack.value, 15000) == WAIT_OBJECT_0, "consumer attach");
      for (int i = 0; i < 128; ++i) {
        ASIOTime time{};
        if (i & 1) require(fake->fire_time_info(i & 1, &time) == &time, "time callback");
        else fake->fire_buffer_switch(i & 1);
        require(output[0].buffers[i & 1] == fake->output_data(0, i & 1), "DAC pointer changed");
        require(std::memcmp(fake->output_data(0, i & 1), expected.data(), plane) == 0 &&
            std::memcmp(fake->output_data(1, i & 1), expected.data() + plane, plane) == 0, "DAC PCM changed");
        require(WaitForSingleObject(ack.value, 15000) == WAIT_OBJECT_0, "consumer PCM ack");
      }
      require(proxy->stop() == ASE_OK && proxy->disposeBuffers() == ASE_OK, "stop/dispose");
      proxy->Release();
    } else {
      HubClient relay(HubRole::relay, scope), obs(HubRole::obs, scope);
      SharedRingReader relay_reader, obs_reader;
      await([&] { relay.ensure_running(); return relay_reader.is_open() || relay_reader.open(relay.mapping_name()); });
      await([&] { return obs_reader.is_open() || obs_reader.open(obs.mapping_name()); });
      SharedRingObserver observer;
      require(observer.open(names.input.c_str()), "observer attach");
      require(relay_reader.format().sample_format == format.wire &&
          obs_reader.format().sample_format == format.wire, "wire format mismatch");
      SetEvent(ack.value);
      for (int i = 0; i < 128; ++i) {
        prepare(i);
        for (auto* reader : {&relay_reader, &obs_reader}) {
          std::array<std::byte, 2048> pcm{}; std::uint32_t count = 0;
          await([&] { return reader->read_planar(pcm.data(), pcm.size(), &count); });
          require(count == static_cast<std::uint32_t>(frames) &&
              std::memcmp(pcm.data(), expected.data(), plane * 2) == 0, "cross-bitness payload changed");
        }
        StreamStatus status{};
        require(observer.snapshot(&status) && status.stats.dropped_blocks == 0, "observer/drop mismatch");
        SetEvent(ack.value);
      }
    }
    std::cout << sizeof(void*) * 8 << "-bit interop passed: " << format_index << "/" << frames << '\n';
    return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
