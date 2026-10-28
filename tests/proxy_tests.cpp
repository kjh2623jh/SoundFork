#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio-bridge/ids.hpp"
#include "audio-bridge/proxy_driver.hpp"
#include "capture-backend/shared_ring.hpp"
#include "fake_asio_driver.hpp"
#include "pcm_fixture.hpp"

namespace {

std::atomic<bool> g_track_allocations{false};
std::atomic<std::uint64_t> g_callback_allocations{0};

struct CallbackState {
  ASIOBufferInfo* buffers = nullptr;
  long buffer_size = 0;
  long switches = 0;
  long time_info_switches = 0;
  ASIOSampleRate changed_rate = 0.0;
};

CallbackState* g_callback_state = nullptr;

void fill_output(long index) noexcept {
  CallbackState& state = *g_callback_state;
  for (long channel = 0; channel < 2; ++channel) {
    auto* samples = static_cast<std::int32_t*>(state.buffers[channel].buffers[index]);
    for (long frame = 0; frame < state.buffer_size; ++frame) {
      samples[frame] = static_cast<std::int32_t>(
          channel * 100000 + state.switches * 100 + frame);
    }
  }
  ++state.switches;
}

void buffer_switch(long index, ASIOBool) {
  fill_output(index);
}

void sample_rate_changed(ASIOSampleRate rate) {
  g_callback_state->changed_rate = rate;
}

long asio_message(long selector, long value, void*, double*) {
  if (selector == kAsioSelectorSupported) {
    return value == kAsioSupportsTimeInfo || value == kAsioResetRequest ||
                   value == kAsioResyncRequest || value == kAsioLatenciesChanged
               ? 1
               : 0;
  }
  return selector == kAsioSupportsTimeInfo ? 1 : 0;
}

ASIOTime* buffer_switch_time_info(ASIOTime* time_info, long index, ASIOBool) {
  ++g_callback_state->time_info_switches;
  fill_output(index);
  return time_info;
}

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ASIOCallbacks callbacks() {
  ASIOCallbacks result{};
  result.bufferSwitch = &buffer_switch;
  result.sampleRateDidChange = &sample_rate_changed;
  result.asioMessage = &asio_message;
  result.bufferSwitchTimeInfo = &buffer_switch_time_info;
  return result;
}

void report_callback_times(const char* mode, std::vector<std::int64_t>& ticks, LONGLONG frequency) {
  const auto total = std::accumulate(ticks.begin(), ticks.end(), std::int64_t{0});
  std::sort(ticks.begin(), ticks.end());
  const auto micros = [frequency](double value) { return value * 1000000.0 / static_cast<double>(frequency); };
  const auto percentile = [&](std::size_t percent) { return micros(static_cast<double>(ticks[(ticks.size() - 1) * percent / 100])); };
  std::cout << "Synthetic callback [" << mode << "] n=" << ticks.size()
            << " mean_us=" << micros(static_cast<double>(total) / static_cast<double>(ticks.size()))
            << " p50_us=" << percentile(50) << " p95_us=" << percentile(95)
            << " p99_us=" << percentile(99) << " max_us=" << micros(static_cast<double>(ticks.back()))
            << " (fake host fill + proxy; not physical latency)\n";
}

void test_zero_copy_and_callbacks() {
  soundfork::test::FakeBackendFactory factory;
  const std::wstring mapping_name =
      L"Local\\SoundFork.ProxyTest." + std::to_wstring(GetCurrentProcessId());
  auto* proxy = new soundfork::ProxyDriver(factory, mapping_name.c_str());

  char name[32]{};
  proxy->getDriverName(name);
  require(std::strcmp(name, "SoundFork") == 0, "unexpected proxy name");
  require(proxy->init(nullptr) == ASIOTrue, "proxy init failed");

  long inputs = -1;
  long outputs = -1;
  require(proxy->getChannels(&inputs, &outputs) == ASE_OK, "getChannels failed");
  require(inputs == 0 && outputs == 2, "unexpected channel count");

  long min_size = 0;
  long max_size = 0;
  long preferred_size = 0;
  long granularity = -1;
  require(
      proxy->getBufferSize(&min_size, &max_size, &preferred_size, &granularity) ==
          ASE_OK,
      "getBufferSize failed");
  require(
      min_size == 64 && max_size == 64 && preferred_size == 64,
      "unexpected fake buffer size");

  ASIOBufferInfo buffers[2]{};
  buffers[0].isInput = ASIOFalse;
  buffers[0].channelNum = 0;
  buffers[1].isInput = ASIOFalse;
  buffers[1].channelNum = 1;
  ASIOCallbacks host_callbacks = callbacks();

  CallbackState callback_state{buffers, 64};
  g_callback_state = &callback_state;
  require(
      proxy->createBuffers(buffers, 2, 64, &host_callbacks) == ASE_OK,
      "createBuffers failed");

  soundfork::test::FakeAsioDriver* fake = factory.last_created();
  if (fake == nullptr) {
    throw std::runtime_error("fake backend was not created");
  }
  for (long channel = 0; channel < 2; ++channel) {
    for (long index = 0; index < 2; ++index) {
      require(
          buffers[channel].buffers[index] == fake->output(channel, index),
          "proxy inserted a playback buffer instead of aliasing backend memory");
    }
  }

  soundfork::capture::SharedRingReader capture_reader;
  require(
      capture_reader.open(mapping_name.c_str()),
      "capture reader did not attach to proxy stream");

  g_callback_allocations.store(0, std::memory_order_relaxed);
  g_track_allocations.store(true, std::memory_order_release);
  fake->fire_buffer_switch(0);
  g_track_allocations.store(false, std::memory_order_release);
  require(
      g_callback_allocations.load(std::memory_order_relaxed) == 0,
      "allocation occurred in the capture callback path");

  std::array<std::int32_t, 128> captured{};
  std::uint32_t captured_frames = 0;
  require(
      capture_reader.read_int32_stereo(
          captured.data(), captured.size(), &captured_frames),
      "proxy did not publish captured audio");
  require(captured_frames == 64, "captured frame count mismatch");
  for (long frame = 0; frame < 64; ++frame) {
    require(captured[frame * 2] == frame, "captured left channel mismatch");
    require(
        captured[frame * 2 + 1] == 100000 + frame,
        "captured right channel mismatch");
  }

  LARGE_INTEGER frequency{};
  require(
      QueryPerformanceFrequency(&frequency) != FALSE,
      "performance counter frequency query failed");
  constexpr long iterations = 10000;
  constexpr long warmup = 128;
  std::vector<std::int64_t> normal_ticks(iterations, 0), saturated_ticks(iterations, 0);
  const auto invoke = [&](long index) {
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    g_track_allocations.store(true, std::memory_order_release);
    QueryPerformanceCounter(&start);
    fake->fire_buffer_switch(index);
    QueryPerformanceCounter(&end);
    g_track_allocations.store(false, std::memory_order_release);
    return end.QuadPart - start.QuadPart;
  };
  const auto consume = [&] {
    require(capture_reader.read_int32_stereo(captured.data(), captured.size(), &captured_frames) &&
            captured_frames == 64, "normal-copy benchmark did not consume its block");
    const auto sequence = callback_state.switches - 1;
    for (long frame = 0; frame < 64; ++frame)
      require(captured[frame * 2] == sequence * 100 + frame &&
              captured[frame * 2 + 1] == 100000 + sequence * 100 + frame,
              "normal-copy benchmark consumed stale/corrupt PCM");
  };
  for (long i = 0; i < warmup; ++i) { invoke(i & 1L); consume(); }
  const auto normal_before = capture_reader.stats();
  for (long i = 0; i < iterations; ++i) {
    normal_ticks[i] = invoke(i & 1L);
    // Consumption and validation are deliberately outside the timed region.
    consume();
  }
  const auto normal_after = capture_reader.stats();
  require(normal_after.published_blocks - normal_before.published_blocks == iterations &&
          normal_after.consumed_blocks - normal_before.consumed_blocks == iterations &&
          normal_after.dropped_blocks == normal_before.dropped_blocks && capture_reader.queued_blocks() == 0,
          "normal benchmark accidentally measured full-ring returns");
  report_callback_times("normal-copy", normal_ticks, frequency.QuadPart);

  for (std::uint32_t i = 0; i < soundfork::capture::kCapacityBlocks; ++i) invoke(static_cast<long>(i & 1));
  require(capture_reader.queued_blocks() == soundfork::capture::kCapacityBlocks, "saturation fixture not full");
  const auto saturated_before = capture_reader.stats();
  for (long i = 0; i < iterations; ++i) saturated_ticks[i] = invoke(i & 1L);
  const auto saturated_after = capture_reader.stats();
  require(saturated_after.published_blocks == saturated_before.published_blocks &&
          saturated_after.consumed_blocks == saturated_before.consumed_blocks &&
          saturated_after.dropped_blocks - saturated_before.dropped_blocks == iterations,
          "saturated benchmark did not exercise the drop path");
  report_callback_times("ring-full/drop", saturated_ticks, frequency.QuadPart);
  std::cout << "Benchmark counters: normal copied/consumed=" << iterations << '/' << iterations
            << " drops=0; saturated copies=0 drops=" << iterations << '\n';
  require(
      g_callback_allocations.load(std::memory_order_relaxed) == 0,
      "allocation occurred in the callback path");
  require(callback_state.switches == 1 + warmup + iterations * 2 + static_cast<long>(soundfork::capture::kCapacityBlocks),
          "callback count mismatch");

  const long last_index = (iterations - 1) & 1L;
  require(
      fake->output(0, last_index)[63] == (callback_state.switches - 1) * 100 + 63,
      "left channel data mismatch");
  require(
      fake->output(1, last_index)[63] == 100000 + (callback_state.switches - 1) * 100 + 63,
      "right channel data mismatch");

  ASIOTime time_info{};
  ASIOTime* returned = fake->fire_time_info(0, &time_info);
  require(returned == &time_info, "time-info return pointer was not forwarded");
  require(callback_state.time_info_switches == 1, "time-info callback not forwarded");

  require(proxy->outputReady() == ASE_OK, "outputReady was not forwarded");
  require(fake->output_ready_calls() == 1, "backend did not receive outputReady");
  require(proxy->start() == ASE_OK, "start failed");
  require(proxy->stop() == ASE_OK, "stop failed");
  require(proxy->disposeBuffers() == ASE_OK, "disposeBuffers failed");
  proxy->Release();
  g_callback_state = nullptr;
}

void test_single_active_stream_guard() {
  soundfork::test::FakeBackendFactory first_factory;
  soundfork::test::FakeBackendFactory second_factory;
  auto* first = new soundfork::ProxyDriver(first_factory);
  auto* second = new soundfork::ProxyDriver(second_factory);
  require(first->init(nullptr) == ASIOTrue, "first init failed");
  require(second->init(nullptr) == ASIOTrue, "second init failed");

  ASIOBufferInfo first_buffers[2]{{ASIOFalse, 0, {nullptr, nullptr}},
                                  {ASIOFalse, 1, {nullptr, nullptr}}};
  ASIOBufferInfo second_buffers[2]{{ASIOFalse, 0, {nullptr, nullptr}},
                                   {ASIOFalse, 1, {nullptr, nullptr}}};
  ASIOCallbacks host_callbacks = callbacks();
  CallbackState callback_state{first_buffers, 64};
  g_callback_state = &callback_state;

  require(
      first->createBuffers(first_buffers, 2, 64, &host_callbacks) == ASE_OK,
      "first createBuffers failed");
  require(
      second->createBuffers(second_buffers, 2, 64, &host_callbacks) ==
          ASE_InvalidMode,
      "second active stream was not rejected");

  require(first->disposeBuffers() == ASE_OK, "first dispose failed");
  first->Release();
  second->Release();
  g_callback_state = nullptr;
}

void test_query_interface() {
  soundfork::test::FakeBackendFactory factory;
  auto* proxy = new soundfork::ProxyDriver(factory);
  void* object = nullptr;
  require(
      proxy->QueryInterface(soundfork::kSoundForkClsid, &object) == S_OK,
      "SoundFork CLSID query failed");
  require(object == static_cast<IASIO*>(proxy), "query returned wrong pointer");
  static_cast<IASIO*>(object)->Release();
  proxy->Release();
}

struct RawCallbackState {
  ASIOBufferInfo* buffers = nullptr;
  soundfork::ProxyDriver* proxy = nullptr;
  std::vector<std::byte> left, right;
  std::size_t plane_bytes = 0;
  bool early_ready = false;
  std::uint64_t switches = 0;
};
RawCallbackState* raw_state = nullptr;
void raw_switch(long index, ASIOBool) {
  std::memcpy(raw_state->buffers[0].buffers[index], raw_state->left.data(), raw_state->plane_bytes);
  std::memcpy(raw_state->buffers[1].buffers[index], raw_state->right.data(), raw_state->plane_bytes);
  ++raw_state->switches;
  if (raw_state->early_ready) raw_state->proxy->outputReady();
}
void raw_rate(ASIOSampleRate) {}
ASIOTime* raw_time(ASIOTime* time, long index, ASIOBool direct) { raw_switch(index, direct); return time; }

void test_raw_formats() {
  using namespace soundfork::capture;
  for (const auto& format : soundfork::test::kPcmCases) {
   for (long block_frames : {32L, 64L, 256L}) {
    soundfork::test::FakeBackendFactory factory({block_frames});
    const auto name = L"Local\\SoundFork.RawProxyTest." + std::to_wstring(GetCurrentProcessId()) +
        L"." + format.name + L"." + std::to_wstring(block_frames);
    auto* proxy = new soundfork::ProxyDriver(factory, name.c_str());
    require(proxy->init(nullptr) == ASIOTrue, "raw init");
    auto* fake = factory.last_created();
    fake->set_channel_type(0, format.asio); fake->set_channel_type(1, format.asio);
    ASIOBufferInfo buffers[2]{}; buffers[1].channelNum = 1;
    ASIOCallbacks host{raw_switch, raw_rate, asio_message, raw_time};
    RawCallbackState state; state.buffers = buffers; state.proxy = proxy;
    state.plane_bytes = static_cast<std::size_t>(block_frames) * format.bytes; raw_state = &state;
    state.left.resize(state.plane_bytes); state.right.resize(state.plane_bytes);
    for (std::size_t i = 0; i < state.plane_bytes; ++i) {
      state.left[i] = static_cast<std::byte>(i & 255); state.right[i] = static_cast<std::byte>((255 - i) & 255);
    }
    require(proxy->createBuffers(buffers, 2, block_frames, &host) == ASE_OK, "raw create");
    require(fake->output_bytes(0) == state.plane_bytes && fake->output_bytes(1) == state.plane_bytes,
            "fake buffers do not match advertised sample width");
    SharedRingReader reader;
    require(reader.open(name.c_str()) && reader.format().sample_format == format.wire, "raw format attach");
    std::vector<std::byte> captured(state.plane_bytes * 2);
    std::uint32_t frames = 0;
    for (int i = 0; i < 4; ++i) {
      state.early_ready = (i & 1) != 0;
      g_track_allocations.store(true);
      ASIOTime time{};
      if (i & 1) require(fake->fire_time_info(i & 1, &time) == &time, "raw time forwarding");
      else fake->fire_buffer_switch(i & 1);
      g_track_allocations.store(false);
      require(buffers[0].buffers[i & 1] == fake->output(0, i & 1), "raw playback not zero-copy");
      require(std::memcmp(fake->output(0, i & 1), state.left.data(), state.plane_bytes) == 0, "DAC bytes changed");
      require(reader.read_planar(captured.data(), captured.size(), &frames) && frames == static_cast<std::uint32_t>(block_frames), "raw capture missing");
      require(std::memcmp(captured.data(), state.left.data(), state.plane_bytes) == 0 &&
              std::memcmp(captured.data() + state.plane_bytes, state.right.data(), state.plane_bytes) == 0, "capture changed original PCM");
      require(!reader.read_planar(captured.data(), captured.size(), &frames), "double publication around outputReady");
    }
    // Stop consuming: callback must keep forwarding both buffers without allocation.
    g_track_allocations.store(true);
    for (int i = 0; i < 100; ++i) fake->fire_buffer_switch(i & 1);
    g_track_allocations.store(false);
    require(reader.stats().dropped_blocks > 0 && state.switches == 104, "slow consumer blocked playback");
    require(g_callback_allocations.load() == 0, "raw callback allocated");
    ASIOSamples position{}; ASIOTimeStamp timestamp{};
    require(fake->getSamplePosition(&position, &timestamp) == ASE_OK &&
            ((static_cast<std::uint64_t>(position.hi) << 32) | position.lo) == state.switches * static_cast<std::uint64_t>(block_frames),
            "fake sample position did not follow configured block size");
    fake->notify_sample_rate(96000);
    require(!reader.stream_active(), "rate change left capture enabled");
    SharedRingObserver observer; StreamStatus status;
    require(observer.open(name.c_str()) && observer.snapshot(&status) &&
            status.capture_error == CaptureError::format_changed, "rate-change diagnostics missing");
    fake->fire_buffer_switch(0);
    require(state.switches == 105, "disabled capture stopped game callback");
    require(proxy->disposeBuffers() == ASE_OK, "raw dispose");
    reader.close(); observer.close();
    fake->notify_sample_rate(48000);
    require(proxy->createBuffers(buffers, 2, block_frames, &host) == ASE_OK && reader.open(name.c_str()), "recreate failed");
    fake->request_reset(); require(!reader.stream_active(), "reset did not disable capture");
    require(proxy->disposeBuffers() == ASE_OK, "reset dispose");
    reader.close();
    // Unsupported and mismatched types still prepare ASIO, but expose no PCM.
    for (bool mismatch : {false, true}) {
      fake->set_channel_type(0, mismatch ? ASIOSTInt16LSB : ASIOSTFloat64LSB);
      fake->set_channel_type(1, mismatch ? ASIOSTInt32LSB : ASIOSTFloat64LSB);
      require(proxy->createBuffers(buffers, 2, block_frames, &host) == ASE_OK, "unsupported playback rejected");
      require(fake->output_bytes(0) == static_cast<std::size_t>(block_frames) * (mismatch ? 2 : 8), "unsupported fixture storage width");
      state.plane_bytes = static_cast<std::size_t>(block_frames) * (mismatch ? 2 : 8);
      state.left.resize(state.plane_bytes); state.right.resize(state.plane_bytes);
      require(!reader.open(name.c_str()), "unsupported capture attached");
      require(observer.open(name.c_str()) && observer.snapshot(&status) &&
              status.capture_error == CaptureError::sample_format, "unsupported reason missing");
      fake->fire_buffer_switch(1);
      observer.close();
      require(proxy->disposeBuffers() == ASE_OK, "unsupported dispose");
    }
    proxy->Release(); raw_state = nullptr;
   }
  }
}

}  // namespace

_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size) {
  if (g_track_allocations.load(std::memory_order_acquire)) {
    g_callback_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}

_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* memory) noexcept {
  std::free(memory);
}

void operator delete[](void* memory) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

int main() {
  try {
    test_query_interface();
    test_zero_copy_and_callbacks();
    test_raw_formats();
    test_single_active_stream_guard();
    require(
        soundfork::ProxyDriver::live_instances() == 0,
        "proxy instance leak detected");
    std::cout << "SoundFork proxy tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    g_track_allocations.store(false, std::memory_order_release);
    std::cerr << "SoundFork proxy test failure: " << error.what() << '\n';
    return 1;
  }
}
