#include <cstdint>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include "obs-plugin/audio_timeline.hpp"
#include "obs-plugin/audio_buffer_policy.hpp"
#include "capture-backend/relay_clock.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void test_late_and_missing_wakes() {
  soundfork::obs::AudioTimeline timeline;
  const std::uint64_t wakes[] = {0, 11000000, 37000000, 39000000, 69000000, 70000000};
  std::uint64_t packets = 0;
  for (const auto now : wakes) {
    const auto batch = timeline.plan(now);
    require(!batch.resynchronized, "ordinary jitter reset the timeline");
    require(batch.packets <= 5, "unbounded catch-up work");
    for (std::uint32_t packet = 0; packet < batch.packets; ++packet) {
      require(timeline.timestamp() == packets * 10000000, "sample timeline has a gap");
      timeline.advance();
      ++packets;
    }
  }
  require(packets == 8, "missed timer notifications slowed the sample clock");
}

void test_long_stall() {
  soundfork::obs::AudioTimeline timeline;
  timeline.plan(1000000000);
  timeline.advance();
  auto batch = timeline.plan(1200000000);
  require(batch.resynchronized && batch.packets == 1, "long stall did not recover once");
  require(timeline.timestamp() == 1200000000, "long stall retained stale timestamps");
  timeline.advance();
  batch = timeline.plan(1201000000);
  require(!batch.resynchronized && batch.packets == 0, "recovery cascaded");
  batch = timeline.plan(1260000000);
  require(!batch.resynchronized && batch.packets == 5, "50 ms boundary was not bounded");
  for (unsigned packet = 0; packet < batch.packets; ++packet) timeline.advance();
  batch = timeline.plan(1260000000);
  require(!batch.resynchronized && batch.packets == 1, "boundary packet was lost");
}

void test_four_hour_jitter() {
  soundfork::obs::AudioTimeline timeline;
  constexpr std::uint64_t duration = 4ULL * 60 * 60 * 1000000000;
  std::uint64_t packets = 0;
  std::uint64_t now = 0;
  for (;;) {
    const auto batch = timeline.plan(now);
    require(!batch.resynchronized, "bounded scheduling jitter caused a reset");
    for (unsigned packet = 0; packet < batch.packets; ++packet) {
      require(timeline.timestamp() == packets * 10000000, "timeline drifted");
      timeline.advance();
      ++packets;
    }
    if (now == duration) break;
    const auto step = packets % 7 == 0 ? 27000000ULL : 10300000ULL;
    now = (std::min)(duration, now + step);
  }
  require(packets == duration / 10000000 + 1, "long-run sample rate changed with timer jitter");
}

void test_prebuffer_and_recovery() {
  soundfork::obs::AudioBufferPolicy policy(64);
  require(policy.supported(), "64-frame stream rejected");
  require(policy.target_frames() == 960, "OBS jitter headroom is not 20 ms");
  require(!policy.ready(960), "startup lacks a complete output packet");
  require(policy.ready(1472), "sufficient prebuffer did not start");
  require(policy.wait_for_input(100, 0), "brief input shortage was zero-filled immediately");
  require(policy.wait_for_input(100, 19000000), "input retry window was too short");
  require(!policy.wait_for_input(483, 19500000), "ready input was delayed");
  require(policy.wait_for_input(0, 20000000), "subsequent input shortage was not bounded separately");
  require(!policy.wait_for_input(0, 40000000), "input retry could wait forever");
  require(policy.ready(500), "normal buffer consumption restarted prebuffering");
  policy.reset();
  require(!policy.wait_for_input(0, 0), "prebuffering incorrectly withheld silence packets");
  require(!policy.ready(500), "underrun did not re-enter prebuffering");
  require(policy.ready(policy.keep_blocks() * 64), "resync keeps too little audio");
  require(!soundfork::obs::AudioBufferPolicy(0).supported(), "zero block size accepted");
  require(!soundfork::obs::AudioBufferPolicy(16).supported(), "undersized ring accepted");
  require(!soundfork::obs::AudioBufferPolicy(8193).supported(), "oversized block accepted");
}

void test_four_hour_capture_drift(double ppm) {
  using soundfork::obs::AudioTimeline;
  soundfork::obs::AudioBufferPolicy policy(64);
  soundfork::capture::RelayClockController clock(48000, 64, policy.target_blocks());
  AudioTimeline timeline;
  double queued = static_cast<double>(policy.startup_frames()) + 64.0;
  std::uint64_t now = 0;
  constexpr std::uint64_t duration = 4ULL * 60 * 60 * 1000000000;
  std::uint64_t packets = 0;
  std::uint64_t wake = 0;
  double final_correction_sum = 0;
  std::uint64_t final_correction_count = 0;
  for (;;) {
    const auto batch = timeline.plan(now);
    require(!batch.resynchronized, "drift/jitter caused a deadline resync");
    require(queued < policy.high_water_frames(), "OBS drift/jitter caused a queue reset");
    for (unsigned packet = 0; packet < batch.packets; ++packet) {
      const auto pending_frames = (batch.packets - packet) * AudioTimeline::packet_frames;
      const auto residual = queued > pending_frames ? queued - pending_frames : 0;
      const auto ratio = clock.update(static_cast<std::uint64_t>(residual), 480);
      if (now >= duration - 60000000000ULL) {
        final_correction_sum += clock.correction_ppm();
        ++final_correction_count;
      }
      require(queued >= 480 * ratio + 2, "OBS drift/jitter caused an underrun");
      queued -= 480 * ratio;
      timeline.advance();
      ++packets;
    }
    if (now == duration) break;
    // Deterministic early wakes, missed notifications and <=20 ms lateness.
    const auto step = ++wake % 11 == 0 ? 27000000ULL : 9300000ULL;
    const auto next = (std::min)(duration, now + step);
    queued += static_cast<double>(next - now) * 48000.0 / 1e9 * (1.0 + ppm * 1e-6);
    now = next;
  }
  require(packets == duration / 10000000 + 1, "OBS sample count drifted");
  std::cout << "OBS drift=" << ppm << " ppm; final-minute mean correction="
            << final_correction_sum / final_correction_count << " ppm\n";
  require(std::abs(final_correction_sum / final_correction_count - ppm) < 30.0,
          "OBS drift controller did not track the producer clock");
}
}  // namespace

int main() {
  try {
    test_late_and_missing_wakes();
    test_long_stall();
    test_four_hour_jitter();
    test_prebuffer_and_recovery();
    test_four_hour_capture_drift(1500.0);
    test_four_hour_capture_drift(-1500.0);
    std::cout << "OBS timeline: catch-up, bounded recovery and virtual four-hour jitter passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
