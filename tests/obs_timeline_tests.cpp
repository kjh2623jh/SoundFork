#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "obs-plugin/audio_timeline.hpp"

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
}  // namespace

int main() {
  try {
    test_late_and_missing_wakes();
    test_long_stall();
    test_four_hour_jitter();
    std::cout << "OBS timeline: catch-up, bounded recovery and virtual four-hour jitter passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
