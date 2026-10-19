#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "audio-bridge/proxy_driver.hpp"
#include "capture-backend/shared_ring.hpp"
#include "fake_asio_driver.hpp"

namespace {

struct CallbackState {
  ASIOBufferInfo* buffers = nullptr;
  std::uint64_t switches = 0;
};

CallbackState* g_state = nullptr;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void buffer_switch(long index, ASIOBool) {
  CallbackState& state = *g_state;
  const std::int32_t base =
      static_cast<std::int32_t>(state.switches & 0x3fffffffU);
  for (long channel = 0; channel < 2; ++channel) {
    auto* samples =
        static_cast<std::int32_t*>(state.buffers[channel].buffers[index]);
    for (long frame = 0; frame < 64; ++frame) {
      samples[frame] = base + channel * 1000 + frame;
    }
  }
  ++state.switches;
}

void sample_rate_changed(ASIOSampleRate) {}

long asio_message(long selector, long value, void*, double*) {
  if (selector == kAsioSelectorSupported) {
    return value == kAsioSupportsTimeInfo ? 1 : 0;
  }
  return selector == kAsioSupportsTimeInfo ? 1 : 0;
}

ASIOTime* buffer_switch_time_info(ASIOTime* time, long index, ASIOBool direct) {
  buffer_switch(index, direct);
  return time;
}

ASIOCallbacks callbacks() {
  ASIOCallbacks result{};
  result.bufferSwitch = &buffer_switch;
  result.sampleRateDidChange = &sample_rate_changed;
  result.asioMessage = &asio_message;
  result.bufferSwitchTimeInfo = &buffer_switch_time_info;
  return result;
}

std::uint64_t private_bytes() noexcept {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
          sizeof(counters)) == FALSE) {
    return 0;
  }
  return static_cast<std::uint64_t>(counters.PrivateUsage);
}

double parse_hours(int argc, wchar_t** argv) {
  double hours = 4.0;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--hours" && index + 1 < argc) {
      wchar_t* end = nullptr;
      hours = std::wcstod(argv[++index], &end);
      if (end == argv[index] || *end != L'\0') {
        throw std::runtime_error("invalid --hours value");
      }
    } else {
      throw std::runtime_error("usage: soundfork_soak_tests [--hours N]");
    }
  }
  if (!std::isfinite(hours) || hours <= 0.0 || hours > 24.0) {
    throw std::runtime_error("--hours must be greater than 0 and at most 24");
  }
  return hours;
}

void run_soak(double hours) {
  constexpr std::uint64_t kCallbacksPerSecond = 48000 / 64;
  const std::uint64_t callback_count = static_cast<std::uint64_t>(
      std::llround(hours * 3600.0 * kCallbacksPerSecond));
  require(callback_count > 0, "soak duration produced no callbacks");

  soundfork::test::FakeBackendFactory factory;
  const std::wstring mapping_name =
      L"Local\\SoundFork.SoakTest." + std::to_wstring(GetCurrentProcessId());
  auto* proxy = new soundfork::ProxyDriver(factory, mapping_name.c_str());
  require(proxy->init(nullptr) == ASIOTrue, "proxy init failed");

  ASIOBufferInfo buffers[2]{{ASIOFalse, 0, {nullptr, nullptr}},
                            {ASIOFalse, 1, {nullptr, nullptr}}};
  ASIOCallbacks host_callbacks = callbacks();
  CallbackState callback_state{buffers};
  g_state = &callback_state;
  require(
      proxy->createBuffers(buffers, 2, 64, &host_callbacks) == ASE_OK,
      "createBuffers failed");
  soundfork::test::FakeAsioDriver* fake = factory.last_created();
  if (fake == nullptr) {
    throw std::runtime_error("fake backend was not created");
  }
  soundfork::capture::SharedRingReader reader;
  require(reader.open(mapping_name.c_str()), "capture reader open failed");
  require(proxy->start() == ASE_OK, "proxy start failed");

  LARGE_INTEGER frequency{};
  require(QueryPerformanceFrequency(&frequency) != FALSE, "QPC unavailable");
  std::array<std::int32_t, 128> captured{};
  std::uint64_t callback_samples = 0;
  std::int64_t total_sampled_ticks = 0;
  std::int64_t maximum_sampled_ticks = 0;
  std::uint64_t consumer_underflows = 0;
  std::uint64_t maximum_queue_blocks = 0;
  const std::uint64_t initial_private_bytes = private_bytes();

  for (std::uint64_t block = 0; block < callback_count; ++block) {
    const bool sample_timing = (block & 0xfffU) == 0;
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    if (sample_timing) {
      QueryPerformanceCounter(&start);
    }
    fake->fire_buffer_switch(static_cast<long>(block & 1U));
    if (sample_timing) {
      QueryPerformanceCounter(&end);
      const std::int64_t elapsed = end.QuadPart - start.QuadPart;
      total_sampled_ticks += elapsed;
      maximum_sampled_ticks = (std::max)(maximum_sampled_ticks, elapsed);
      ++callback_samples;
    }
    maximum_queue_blocks =
        (std::max)(maximum_queue_blocks, reader.queued_blocks());
    std::uint32_t frames_read = 0;
    if (!reader.read_int32_stereo(
            captured.data(), captured.size(), &frames_read) ||
        frames_read != 64) {
      ++consumer_underflows;
    }
  }

  const auto stats = reader.stats();
  const std::uint64_t final_private_bytes = private_bytes();
  require(proxy->stop() == ASE_OK, "proxy stop failed");
  require(proxy->disposeBuffers() == ASE_OK, "disposeBuffers failed");
  proxy->Release();
  g_state = nullptr;

  require(callback_state.switches == callback_count, "callback count mismatch");
  require(stats.published_blocks == callback_count, "published count mismatch");
  require(stats.consumed_blocks == callback_count, "consumed count mismatch");
  require(stats.dropped_blocks == 0, "producer dropped capture blocks");
  require(consumer_underflows == 0, "synthetic consumer underflowed");
  require(maximum_queue_blocks <= 1, "synthetic relay queue grew");
  require(
      soundfork::ProxyDriver::live_instances() == 0,
      "proxy instance leak detected");

  const double average_microseconds =
      1000000.0 * static_cast<double>(total_sampled_ticks) /
      (static_cast<double>(frequency.QuadPart) * callback_samples);
  const double maximum_microseconds =
      1000000.0 * static_cast<double>(maximum_sampled_ticks) /
      static_cast<double>(frequency.QuadPart);
  std::cout << "Accelerated ASIO time: " << hours << " hours\n"
            << "Callbacks: " << callback_count << "\n"
            << "Sampled callback wall time (average/max): "
            << average_microseconds << "/" << maximum_microseconds << " us\n"
            << "Producer drops: " << stats.dropped_blocks << "\n"
            << "Consumer underflows: " << consumer_underflows << "\n"
            << "Maximum relay queue: " << maximum_queue_blocks << " blocks\n"
            << "Private bytes (start/end): " << initial_private_bytes << "/"
            << final_private_bytes << "\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    run_soak(parse_hours(argc, argv));
    std::cout << "SoundFork accelerated soak test passed\n";
    return 0;
  } catch (const std::exception& error) {
    g_state = nullptr;
    std::cerr << "SoundFork soak test failure: " << error.what() << '\n';
    return 1;
  }
}
