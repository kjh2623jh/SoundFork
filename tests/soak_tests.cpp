#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "audio-bridge/proxy_driver.hpp"
#include "capture-backend/shared_ring.hpp"
#include "fake_asio_driver.hpp"
#include "pcm_fixture.hpp"

namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }

struct CallbackState {
  ASIOBufferInfo* buffers = nullptr;
  const soundfork::test::PcmCase* format = nullptr;
  std::uint32_t frames = 64;
  std::uint64_t switches = 0;
};
CallbackState* g_state = nullptr;

void buffer_switch(long index, ASIOBool) {
  for (std::uint32_t channel = 0; channel < 2; ++channel)
    soundfork::test::fill_pcm(g_state->buffers[channel].buffers[index], *g_state->format,
                            g_state->frames, g_state->switches, channel);
  ++g_state->switches;
}
void sample_rate_changed(ASIOSampleRate) {}
long asio_message(long selector, long value, void*, double*) {
  return selector == kAsioSupportsTimeInfo ||
      (selector == kAsioSelectorSupported && value == kAsioSupportsTimeInfo) ? 1 : 0;
}
ASIOTime* time_info(ASIOTime* time, long index, ASIOBool direct) {
  buffer_switch(index, direct); return time;
}
std::uint64_t private_bytes() noexcept {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                            sizeof(counters))) return 0;
  return static_cast<std::uint64_t>(counters.PrivateUsage);
}

struct Options {
  double hours = 4.0;  // Backward-compatible manual default; CTest explicitly chooses 30 seconds.
  std::wstring_view format = L"int32";
  long frames = 64;
};
Options parse_options(int argc, wchar_t** argv) {
  Options options;
  bool duration_set = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view argument(argv[i]);
    require(i + 1 < argc, "missing option value");
    if (argument == L"--hours" || argument == L"--seconds") {
      require(!duration_set, "choose either --hours or --seconds once");
      duration_set = true;
      wchar_t* end = nullptr;
      const auto value = std::wcstod(argv[++i], &end);
      require(end != argv[i] && *end == L'\0' && std::isfinite(value), "invalid duration");
      options.hours = argument == L"--hours" ? value : value / 3600.0;
    } else if (argument == L"--format") options.format = argv[++i];
    else if (argument == L"--buffer-size") {
      wchar_t* end = nullptr;
      options.frames = std::wcstol(argv[++i], &end, 10);
      require(end != argv[i] && *end == L'\0', "invalid block size");
    } else throw std::runtime_error("usage: soundfork_soak_tests [--hours N | --seconds N] [--format int16|int24|int32|float32|all] [--buffer-size N]");
  }
  require(options.hours > 0 && options.hours <= 24, "duration must be greater than zero and at most 24 hours");
  require(options.frames >= 1 && options.frames <= 8192, "block size must be 1..8192");
  bool known = options.format == L"all";
  for (const auto& format : soundfork::test::kPcmCases) known = known || options.format == format.name;
  require(known, "unsupported test format");
  return options;
}

void run_soak(const Options& options, const soundfork::test::PcmCase& format) {
  const auto callbacks = static_cast<std::uint64_t>(std::llround(options.hours * 3600.0 * 48000.0 / options.frames));
  require(callbacks > 0, "duration produced no callbacks");
  std::wcout << L"START accelerated format=" << format.name << L" frames/block=" << options.frames
             << L" simulated_seconds=" << options.hours * 3600.0 << std::endl;
  soundfork::test::FakeBackendFactory factory({options.frames, 48000.0, {format.asio, format.asio}});
  const auto mapping = L"Local\\SoundFork.SoakTest." + std::to_wstring(GetCurrentProcessId());
  auto* proxy = new soundfork::ProxyDriver(factory, mapping.c_str());
  struct Release { soundfork::ProxyDriver* value; ~Release() { value->Release(); } } owner{proxy};
  require(proxy->init(nullptr) == ASIOTrue, "proxy init failed");
  ASIOBufferInfo buffers[2]{{ASIOFalse, 0, {nullptr, nullptr}}, {ASIOFalse, 1, {nullptr, nullptr}}};
  ASIOCallbacks callbacks_to_host{buffer_switch, sample_rate_changed, asio_message, time_info};
  CallbackState state{buffers, &format, static_cast<std::uint32_t>(options.frames)};
  g_state = &state;
  require(proxy->createBuffers(buffers, 2, options.frames, &callbacks_to_host) == ASE_OK, "createBuffers failed");
  auto* fake = factory.last_created();
  require(fake != nullptr, "fake backend missing");
  soundfork::capture::SharedRingReader reader;
  require(reader.open(mapping.c_str()), "reader open failed");
  require(reader.format().sample_format == format.wire, "wrong wire format");
  require(proxy->start() == ASE_OK, "start failed");
  LARGE_INTEGER frequency{};
  require(QueryPerformanceFrequency(&frequency) != FALSE, "QPC unavailable");
  const auto plane = static_cast<std::size_t>(options.frames) * format.bytes;
  std::vector<std::byte> captured(plane * 2, std::byte{0});
  const bool integer_reader = format.wire == soundfork::capture::SampleFormat::s32le;
  // Preserve the original long-soak Int32 reader/decoder coverage as well as
  // exercising raw transport for the newly added native formats.
  std::vector<std::int32_t> decoded(integer_reader ? static_cast<std::size_t>(options.frames) * 2 : 0, 0);
  const auto initial_memory = private_bytes();
  require(initial_memory != 0, "memory measurement unavailable");
  std::uint64_t underflows = 0, maximum_queue = 0, sampled_callbacks = 0;
  LONGLONG total_ticks = 0, maximum_ticks = 0;
  for (std::uint64_t block = 0; block < callbacks; ++block) {
    const auto index = static_cast<long>(block & 1);
    const bool sampled = (block & 0xfff) == 0;
    LARGE_INTEGER before{}, after{};
    ASIOTime time{};
    if (sampled) QueryPerformanceCounter(&before);
    if (block & 1) fake->fire_time_info(index, &time);
    else fake->fire_buffer_switch(index);
    if (sampled) {
      QueryPerformanceCounter(&after);
      total_ticks += after.QuadPart - before.QuadPart;
      maximum_ticks = (std::max)(maximum_ticks, after.QuadPart - before.QuadPart);
      ++sampled_callbacks;
    }
    maximum_queue = (std::max)(maximum_queue, reader.queued_blocks());
    std::uint32_t frames = 0;
    const bool read = integer_reader ? reader.read_int32_stereo(decoded.data(), decoded.size(), &frames)
                                    : reader.read_planar(captured.data(), captured.size(), &frames);
    if (!read || frames != state.frames) {
      ++underflows;
    } else if (integer_reader) {
      for (std::uint32_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t channel = 0; channel < 2; ++channel) {
          std::int32_t expected = 0;
          const auto* native = static_cast<const std::byte*>(fake->output_data(static_cast<long>(channel), index));
          std::memcpy(&expected, native + static_cast<std::size_t>(frame) * 4, 4);
          require(decoded[static_cast<std::size_t>(frame) * 2 + channel] == expected,
                  "Int32 consumer changed PCM or channel order");
        }
      }
    } else {
      require(std::memcmp(captured.data(), fake->output_data(0, index), plane) == 0 &&
              std::memcmp(captured.data() + plane, fake->output_data(1, index), plane) == 0,
              "stale/corrupt PCM or swapped channels");
    }
  }
  const auto stats = reader.stats();
  const auto final_memory = private_bytes();
  require(proxy->stop() == ASE_OK && proxy->disposeBuffers() == ASE_OK, "stop/dispose failed");
  g_state = nullptr;
  require(state.switches == callbacks && stats.published_blocks == callbacks &&
          stats.consumed_blocks == callbacks, "callback/publication/consumption count mismatch");
  require(stats.dropped_blocks == 0 && underflows == 0, "drop or underflow in normal-copy soak");
  require(maximum_queue <= 1, "queue grew in synchronous accelerated fixture");
  require(final_memory != 0, "final memory measurement unavailable");
  std::wcout << L"END format=" << format.name << L" callbacks=" << callbacks
             << L" normal_copy_sampled_mean_us=" << 1000000.0 * static_cast<double>(total_ticks) /
                    (static_cast<double>(frequency.QuadPart) * static_cast<double>(sampled_callbacks))
             << L" max_us=" << 1000000.0 * static_cast<double>(maximum_ticks) / static_cast<double>(frequency.QuadPart)
             << L" producer_drops=" << stats.dropped_blocks << L" consumer_underflows=" << underflows
             << L" max_queue_blocks=" << maximum_queue
             << L" private_bytes_start=" << initial_memory << L" end=" << final_memory << std::endl;
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    const auto options = parse_options(argc, argv);
    for (const auto& format : soundfork::test::kPcmCases)
      if (options.format == L"all" || options.format == format.name) run_soak(options, format);
    require(soundfork::ProxyDriver::live_instances() == 0, "proxy instance leak");
    std::cout << "Accelerated synthetic soak passed; not real-time hardware or physical latency validation\n";
    return 0;
  } catch (const std::exception& error) {
    g_state = nullptr;
    std::cerr << error.what() << '\n'; return 1;
  }
}
