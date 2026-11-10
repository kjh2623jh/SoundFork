#include <windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "capture-backend/relay_clock.hpp"
#include "capture-backend/relay_preferences.hpp"
#include "capture-backend/endpoint_selection.hpp"
#include "capture-backend/relay_recovery.hpp"
#include "capture-backend/shared_ring.hpp"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_four_hour_drift(double producer_ppm, std::uint32_t kSampleRate) {
  constexpr std::uint32_t kBlockFrames = 64;
  const std::uint32_t kEventFrames = kSampleRate / 100;
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

// Model the real ownership boundary: 64-frame ASIO packets arrive between
// WASAPI events; a complete render batch must be available at each event.
void test_packetized_render(std::uint32_t target, std::uint32_t batch, double ppm,
    bool catchup = false) {
  using namespace soundfork::capture;
  const auto name = L"Local\\SoundFork.PacketizedRelay." + std::to_wstring(GetCurrentProcessId());
  SharedRingPublisher publisher;
  require(publisher.open(48000, 64, name.c_str()), "packetized publisher");
  SharedRingReader reader;
  require(reader.open(name.c_str(), false), "packetized reader");
  auto source = std::make_unique<RelayAudioSource>(reader.format());
  RelayRenderClock clock(48000, 64, target);
  std::array<std::int32_t, 64> left{}, right{};
  left.fill(131072); right.fill(-65536);
  std::array<std::int32_t, 1920> output{};
  double incoming = 0;
  bool started = false;
  std::uint64_t rendered = 0;
  const auto events = 30U * 48000U / batch;
  for (std::uint32_t event = 0; event < events; ++event) {
    const auto requested = catchup && event % 11 == 0 ? 960U : batch;
    incoming += requested * (1.0 + ppm * 0.000001);
    while (incoming >= 64) {
      require(publisher.publish_int32_stereo(left.data(), right.data()), "packetized ring overflow");
      incoming -= 64;
    }
    const auto queued = source->buffered_frames(reader);
    const auto decision = clock.prepare(queued, requested);
    if (decision.rebuffered) throw std::runtime_error("steady rebuffer: target=" +
        std::to_string(target) + " batch=" + std::to_string(batch) + " ppm=" +
        std::to_string(ppm) + " event=" + std::to_string(event));
    if (!decision.render) {
      require(!started, "steady packetized source inserted a silent batch");
      continue;
    }
    started = true;
    require(source->render(reader, output.data(), requested, decision.ratio), "packetized render silent");
    require(source->metrics().silent_frames == 0, "WASAPI batch exhausted the ASIO packet queue");
    for (std::uint32_t frame = 0; frame < requested; ++frame)
      require(output[frame * 2] == left[0] && output[frame * 2 + 1] == right[0],
          "packetized PCM contains a gap or corrupt sample");
    rendered += requested;
  }
  require(rendered > 20U * 48000U, "packetized render never left prebuffering");
}

void test_relay_rebuffer_recovery() {
  using namespace soundfork::capture;
  const auto name = L"Local\\SoundFork.RelayRecovery." + std::to_wstring(GetCurrentProcessId());
  SharedRingPublisher publisher;
  require(publisher.open(48000, 64, name.c_str()), "rebuffer publisher");
  SharedRingReader reader;
  require(reader.open(name.c_str(), false), "rebuffer reader");
  auto source = std::make_unique<RelayAudioSource>(reader.format());
  RelayRenderClock clock(48000, 64, 256);
  std::array<std::int32_t, 64> samples{}; samples.fill(65536);
  std::array<std::int32_t, 960> output{};
  const auto publish = [&](unsigned blocks) {
    for (unsigned block = 0; block < blocks; ++block)
      require(publisher.publish_int32_stereo(samples.data(), samples.data()), "rebuffer publish");
  };
  publish(12);
  auto decision = clock.prepare(source->buffered_frames(reader), 480);
  require(decision.render, "rebuffer initial fill did not start");
  require(source->render(reader, output.data(), 480, decision.ratio), "rebuffer initial render");
  // No new producer packets: preserve remaining PCM instead of repeatedly
  // draining tiny fragments and generating one discontinuity per WASAPI event.
  const auto remaining = source->buffered_frames(reader);
  decision = clock.prepare(remaining, 480);
  require(!decision.render && decision.rebuffered, "starvation did not restart buffering");
  decision = clock.prepare(remaining, 480);
  require(!decision.render && !decision.rebuffered, "starvation counted more than once");
  require(source->buffered_frames(reader) == remaining, "buffering discarded pending PCM");
  publish(8);
  decision = clock.prepare(source->buffered_frames(reader), 480);
  require(decision.render && !decision.rebuffered, "recovered source did not restart");
  require(source->render(reader, output.data(), 480, decision.ratio), "recovered render missing");
  require(source->metrics().silent_frames == 0, "recovered render contains partial underrun");
  clock.reset();
  require(!clock.prepare(source->buffered_frames(reader), 480).render,
      "explicit resync skipped prebuffering");
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

void test_all_consumer_formats() {
  using namespace soundfork::capture;
  const auto name = L"Local\\SoundFork.ConsumerFormats." + std::to_wstring(GetCurrentProcessId());
  for (auto format : {SampleFormat::s16le, SampleFormat::s24le, SampleFormat::s32le, SampleFormat::f32le}) {
    SharedRingPublisher publisher;
    require(publisher.open(48000, 64, name.c_str(), format), "consumer format publisher");
    SharedRingReader reader;
    require(reader.open(name.c_str(), false), "consumer format reader");
    std::array<std::byte, 256> left{}, right{};
    const auto width = sample_bytes(format);
    for (std::size_t i = 0; i < 64; ++i) {
      if (format == SampleFormat::f32le) {
        const float l = 0.25F, r = -0.125F;
        std::memcpy(left.data() + i * width, &l, 4); std::memcpy(right.data() + i * width, &r, 4);
      } else {
        // LE representations of +1/4 and -1/8 full scale at each width.
        const auto l = 1U << (width * 8 - 3), r = 15U << (width * 8 - 4);
        for (std::uint32_t byte = 0; byte < width; ++byte) {
          left[i * width + byte] = static_cast<std::byte>((l >> (8 * byte)) & 255);
          right[i * width + byte] = static_cast<std::byte>((r >> (8 * byte)) & 255);
        }
      }
    }
    for (int block = 0; block < 8; ++block) require(publisher.publish_planar(left.data(), right.data()), "consumer format publish");
    auto integer = std::make_unique<RelayAudioSource>(reader.format());
    std::array<std::int32_t, 128> integer_output{};
    require(integer->render(reader, integer_output.data(), 64, 1.0001), "integer renderer missing audio");
    for (std::size_t i = 0; i < 64; ++i)
      require(integer_output[i * 2] == 536870912 && integer_output[i * 2 + 1] == -268435456, "consumer normalization changed");
    require(integer->metrics().silent_frames == 0, "integer underrun");
    auto floating = std::make_unique<RelayFloatAudioSource>(reader.format());
    std::array<float, 128> float_output{};
    require(floating->render(reader, float_output.data(), 64, 0.9999), "float renderer missing audio");
    for (std::size_t i = 0; i < 64; ++i)
      require(float_output[i * 2] == 0.25F && float_output[i * 2 + 1] == -0.125F, "float renderer normalization changed");
  }
  SharedRingPublisher publisher;
  require(publisher.open(48000, 64, name.c_str(), SampleFormat::f32le), "tiny float publisher");
  SharedRingReader reader; require(reader.open(name.c_str(), false), "tiny float reader");
  std::array<float, 64> tiny{}, large{}; tiny.fill(0x1p-40F); large.fill(2.0F);
  require(publisher.publish_planar(tiny.data(), large.data()) && publisher.publish_planar(tiny.data(), large.data()), "tiny float publish");
  auto floating = std::make_unique<RelayFloatAudioSource>(reader.format());
  std::array<float, 64> output{};
  require(floating->render(reader, output.data(), 32, 1.0), "tiny float render");
  require(output[0] == 0x1p-40F && output[1] == 2.0F, "Float32 went through an integer conversion or clipping");
}
}  // namespace

int main() {
  try {
    require(soundfork::capture::relay_target_frames(64, 0) == 256, "auto target");
    require(soundfork::capture::relay_target_frames(64, 960) == 960, "frame target");
    require(soundfork::capture::relay_target_frames(64, 9600) == 2048, "ring headroom");
    require(soundfork::capture::relay_target_frames(512, 128) == 512, "minimum block");
    require(soundfork::capture::relay_target_frames(64, 0xffffffffUL) == 256, "invalid target fallback");
    require(soundfork::capture::RelayClockController(48000, 64).target_frames() == 256,
        "default relay target changed");
    require(soundfork::capture::RelayClockController(48000, 64, 4, 960).target_frames() == 960,
        "explicit frame target ignored");
    for (auto rate : soundfork::capture::kCaptureSampleRates) {
      test_four_hour_drift(1500.0, rate);
      test_four_hour_drift(-1500.0, rate);
    }
    test_linear_relay_source();
    for (auto target : {128U, 256U, 960U})
      for (auto batch : {480U, 960U})
        for (auto ppm : {-1500.0, 0.0, 1500.0}) test_packetized_render(target, batch, ppm);
    for (auto target : {128U, 256U, 960U})
      for (auto ppm : {-1500.0, 0.0, 1500.0}) test_packetized_render(target, 480, ppm, true);
    test_relay_rebuffer_recovery();
    test_all_consumer_formats();
    test_reconnect_error_policy();
    std::cout << "SoundFork relay clock tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "SoundFork relay clock test failure: " << error.what() << '\n';
    return 1;
  }
}
