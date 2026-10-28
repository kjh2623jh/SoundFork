#include <windows.h>
#include <avrt.h>
#include <obs-module.h>
#include <util/platform.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include "capture-backend/hub_client.hpp"
#include "capture-backend/relay_clock.hpp"
#include "obs-plugin/audio_timeline.hpp"
#include "obs-plugin/audio_buffer_policy.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("soundfork-obs", "en-US")
OBS_MODULE_AUTHOR("SoundFork contributors")

namespace {
std::atomic<bool> source_exists{false};

struct Source {
  explicit Source(obs_source_t* value) : source(value) {}
  ~Source() {
    if (stop != nullptr) SetEvent(stop);
    if (worker != nullptr) {
      WaitForSingleObject(worker, INFINITE);
      CloseHandle(worker);
    }
    if (supervisor != nullptr) {
      WaitForSingleObject(supervisor, INFINITE);
      CloseHandle(supervisor);
    }
    if (stop != nullptr) CloseHandle(stop);
  }
  obs_source_t* source;
  HANDLE stop = nullptr;
  HANDLE worker = nullptr;
  HANDLE supervisor = nullptr;
};

struct CaptureMetrics {
  unsigned long long packets = 0;
  unsigned long long idle_silence_frames = 0;
  unsigned long long prebuffer_silence_frames = 0;
  unsigned long long underrun_silence_frames = 0;
  unsigned long long discarded_blocks = 0;
  unsigned long long deadline_resyncs = 0;
  unsigned long long queue_resyncs = 0;
  unsigned long long underruns = 0;
  unsigned long long attachments = 0;
  unsigned long long disconnects = 0;
  unsigned long long prebuffer_starts = 0;
  unsigned long long catchup_packets = 0;
  unsigned long long input_wait_wakes = 0;
  unsigned long long maximum_queue = 0;
  unsigned long long maximum_lateness_ns = 0;
  unsigned long long hub_output_drops = 0;
  double maximum_ppm = 0;

  void log(const char* phase, std::uint64_t queued) const {
    blog(LOG_INFO,
         "[SoundFork] %s packets=%llu queue_frames=%llu max_queue_frames=%llu "
         "idle_silence_frames=%llu prebuffer_silence_frames=%llu underrun_silence_frames=%llu "
         "discarded_blocks=%llu deadline_resyncs=%llu queue_resyncs=%llu underruns=%llu "
         "attachments=%llu disconnects=%llu prebuffer_starts=%llu catchup_packets=%llu input_wait_wakes=%llu "
         "max_lateness_us=%llu hub_output_drops=%llu max_correction_ppm=%.2f",
         phase, packets, static_cast<unsigned long long>(queued), maximum_queue,
         idle_silence_frames, prebuffer_silence_frames, underrun_silence_frames,
         discarded_blocks, deadline_resyncs, queue_resyncs, underruns,
         attachments, disconnects, prebuffer_starts, catchup_packets, input_wait_wakes,
         maximum_lateness_ns / 1000, hub_output_drops, maximum_ppm);
  }
};

struct AudioScheduling {
  AudioScheduling() {
    DWORD task_index = 0;
    handle = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
    if (handle == nullptr)
      blog(LOG_WARNING, "[SoundFork] MMCSS Audio registration failed: %lu", GetLastError());
  }
  ~AudioScheduling() { if (handle != nullptr) AvRevertMmThreadCharacteristics(handle); }
  HANDLE handle = nullptr;
};

// Selection and allocation happen only on this capture worker when attaching.
// Float sources never travel through Int32; both variants share timing policy.
class ObsAudioSource {
 public:
  explicit ObsAudioSource(const soundfork::capture::StreamFormat& format) {
    if (format.sample_format == soundfork::capture::SampleFormat::f32le)
      floating_ = std::make_unique<soundfork::capture::RelayFloatAudioSource>(format);
    else integer_ = std::make_unique<soundfork::capture::RelayAudioSource>(format);
  }
  bool is_float() const noexcept { return floating_ != nullptr; }
  void reset() noexcept { if (floating_) floating_->reset(); else integer_->reset(); }
  std::uint64_t buffered_frames(const soundfork::capture::SharedRingReader& reader) const noexcept {
    return floating_ ? floating_->buffered_frames(reader) : integer_->buffered_frames(reader);
  }
  soundfork::capture::RelaySourceMetrics metrics() const noexcept {
    return floating_ ? floating_->metrics() : integer_->metrics();
  }
  void render(soundfork::capture::SharedRingReader& reader, std::int32_t* integer,
              float* floating, std::uint32_t frames, double ratio) noexcept {
    if (floating_) floating_->render(reader, floating, frames, ratio);
    else integer_->render(reader, integer, frames, ratio);
  }
 private:
  std::unique_ptr<soundfork::capture::RelayAudioSource> integer_;
  std::unique_ptr<soundfork::capture::RelayFloatAudioSource> floating_;
};

void capture(Source& state) {
  using namespace soundfork::capture;
  HubClient hub(HubRole::obs);
  SharedRingReader reader;
  std::unique_ptr<ObsAudioSource> audio;
  std::unique_ptr<RelayClockController> clock;
  std::unique_ptr<soundfork::obs::AudioBufferPolicy> buffering;
  constexpr std::uint32_t frames = 480;
  std::array<std::int32_t, frames * 2> samples{};
  std::array<float, frames * 2> float_samples{};
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
  if (timer == nullptr) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  if (timer == nullptr) {
    blog(LOG_ERROR, "[SoundFork] Cannot create capture timer");
    return;
  }
  struct TimerCloser {
    HANDLE value;
    ~TimerCloser() { CloseHandle(value); }
  } closer{timer};
  const HANDLE waits[] = {state.stop, timer};
  AudioScheduling scheduling;
  soundfork::obs::AudioTimeline timeline;
  std::uint64_t next_probe = 0;
  std::uint64_t next_report = os_gettime_ns() + 30000000000;
  std::uint64_t last_hub_drops = 0;
  CaptureMetrics metrics;
  bool prebuffered = false;
  const auto reset_capture = [&] {
    if (!audio || !clock || !buffering) return;
    metrics.discarded_blocks += reader.discard_oldest(buffering->keep_blocks());
    audio->reset();
    clock->reset();
    buffering->reset();
    prebuffered = false;
  };
  while (WaitForSingleObject(state.stop, 0) == WAIT_TIMEOUT) {
    const auto probe_now = os_gettime_ns();
    if (reader.is_open()) {
      const auto drops = reader.stats().dropped_blocks;
      metrics.hub_output_drops += drops >= last_hub_drops ? drops - last_hub_drops : drops;
      last_hub_drops = drops;
    }
    if (probe_now >= next_probe) {
      next_probe = probe_now + 250000000;
      if (reader.is_open() && !reader.stream_active()) {
        reader.close();
        audio.reset();
        clock.reset();
        buffering.reset();
        prebuffered = false;
        ++metrics.disconnects;
        blog(LOG_INFO, "[SoundFork] Stream disconnected; waiting for a producer");
      }
      if (!reader.is_open()) {
        if (reader.open(hub.mapping_name())) {
          const auto format = reader.format();
          if (format.sample_rate != 48000 || format.channel_count != 2) {
            reader.close();
          } else {
            buffering = std::make_unique<soundfork::obs::AudioBufferPolicy>(format.frames_per_block);
            if (!buffering->supported()) {
              blog(LOG_WARNING, "[SoundFork] %u-frame blocks cannot hold the OBS jitter buffer",
                   format.frames_per_block);
              reader.close();
              buffering.reset();
              continue;
            }
            audio = std::make_unique<ObsAudioSource>(format);
            clock = std::make_unique<RelayClockController>(
                format.sample_rate, format.frames_per_block, buffering->target_blocks());
            ++metrics.attachments;
            last_hub_drops = reader.stats().dropped_blocks;
            blog(LOG_INFO, "[SoundFork] Attached: %u Hz, %u frames/block, target=%llu frames",
                 format.sample_rate, format.frames_per_block,
                 static_cast<unsigned long long>(buffering->target_frames()));
          }
        }
      }
    }
    // A timer signal is not a sample clock: service every elapsed packet
    // deadline, even if Windows coalesced or delayed wake-ups.
    const auto now = os_gettime_ns();
    const auto batch = timeline.plan(now);
    metrics.maximum_lateness_ns = (std::max)(metrics.maximum_lateness_ns, batch.lateness_ns);
    if (batch.resynchronized) {
      ++metrics.deadline_resyncs;
      reset_capture();
    }
    bool waiting_for_input = false;
    for (std::uint32_t packet_index = 0; packet_index < batch.packets; ++packet_index) {
      if (WaitForSingleObject(state.stop, 0) != WAIT_TIMEOUT) break;
      samples.fill(0);
      float_samples.fill(0);
      if (reader.is_open() && audio && clock && buffering) {
        auto queued = audio->buffered_frames(reader);
        metrics.maximum_queue = (std::max)(metrics.maximum_queue, queued);
        if (queued > buffering->high_water_frames()) {
          reset_capture();
          queued = audio->buffered_frames(reader);
          ++metrics.queue_resyncs;
        }
        if (!prebuffered && buffering->ready(queued)) {
          prebuffered = true;
          ++metrics.prebuffer_starts;
        }
        if (prebuffered) {
          if (buffering->wait_for_input(queued, os_gettime_ns())) {
            ++metrics.input_wait_wakes;
            waiting_for_input = true;
            break;
          }
          // Remove all due output packets before evaluating residual headroom;
          // catch-up backlog is scheduling jitter, not hardware clock drift.
          const auto pending_frames = (batch.packets - packet_index) * frames;
          const double ratio = clock->update(
              queued > pending_frames ? queued - pending_frames : 0, frames);
          metrics.maximum_ppm = (std::max)(metrics.maximum_ppm, std::abs(clock->correction_ppm()));
          const auto before = audio->metrics().silent_frames;
          audio->render(reader, samples.data(), float_samples.data(), frames, ratio);
          const auto missing = audio->metrics().silent_frames - before;
          metrics.underrun_silence_frames += missing;
          if (missing != 0) {
            ++metrics.underruns;
            // Refill after a real underrun instead of alternating short audio
            // fragments and zeros on every subsequent packet.
            audio->reset();
            clock->reset();
            buffering->reset();
            prebuffered = false;
          }
        } else {
          metrics.prebuffer_silence_frames += frames;
        }
      } else {
        metrics.idle_silence_frames += frames;
      }
      obs_source_audio packet{};
      const bool float_output = audio && audio->is_float();
      packet.data[0] = float_output ? reinterpret_cast<const std::uint8_t*>(float_samples.data())
                                   : reinterpret_cast<const std::uint8_t*>(samples.data());
      packet.frames = frames;
      packet.speakers = SPEAKERS_STEREO;
      packet.format = float_output ? AUDIO_FORMAT_FLOAT : AUDIO_FORMAT_32BIT;
      packet.samples_per_sec = 48000;
      packet.timestamp = timeline.timestamp();
      obs_source_output_audio(state.source, &packet);
      timeline.advance();
      if (packet_index != 0) ++metrics.catchup_packets;
      ++metrics.packets;
    }
    if (now >= next_report) {
      metrics.log("periodic (cumulative)", audio ? audio->buffered_frames(reader) : 0);
      next_report = now + 30000000000;
    }
    // Rearm relative to the absolute sample deadline, not relative to the last
    // wake. A one-shot timer cannot silently lose a count of audio periods.
    const auto after = os_gettime_ns();
    const auto remaining = waiting_for_input ? 1000000 :
        (timeline.timestamp() > after ? timeline.timestamp() - after : 100);
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>((remaining + 99) / 100);
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      blog(LOG_ERROR, "[SoundFork] Cannot arm capture timer");
      break;
    }
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
  }
  metrics.log("final (cumulative)", audio ? audio->buffered_frames(reader) : 0);
}

DWORD WINAPI worker_main(void* context) noexcept {
  try {
    capture(*static_cast<Source*>(context));
  } catch (...) {
    blog(LOG_ERROR, "[SoundFork] Capture worker stopped after an exception");
  }
  return 0;
}

DWORD WINAPI supervisor_main(void* context) noexcept {
  auto& state = *static_cast<Source*>(context);
  try {
    // CreateProcess can take hundreds of milliseconds on a busy system. Keep
    // process launch/recovery off the timed OBS audio delivery worker as well
    // as off the ASIO callback.
    soundfork::capture::HubClient hub(soundfork::capture::HubRole::obs);
    do {
      hub.ensure_running();
    } while (WaitForSingleObject(state.stop, 250) == WAIT_TIMEOUT);
  } catch (...) {
    blog(LOG_ERROR, "[SoundFork] Capture hub supervisor stopped after an exception");
  }
  return 0;
}

const char* source_name(void*) { return obs_module_text("SourceName"); }

void* create_source(obs_data_t*, obs_source_t* source) noexcept {
  bool expected = false;
  if (!source_exists.compare_exchange_strong(expected, true)) {
    blog(LOG_WARNING, "[SoundFork] Add an existing SoundFork source instead of creating a second one");
    return nullptr;
  }
  try {
    auto state = std::make_unique<Source>(source);
    state->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state->stop != nullptr) {
      state->supervisor = CreateThread(nullptr, 0, supervisor_main, state.get(), 0, nullptr);
      state->worker = CreateThread(nullptr, 0, worker_main, state.get(), 0, nullptr);
    }
    if (state->worker != nullptr && state->supervisor != nullptr) return state.release();
  } catch (...) {
    blog(LOG_ERROR, "[SoundFork] Cannot allocate capture source");
  }
  source_exists.store(false);
  return nullptr;
}

void destroy_source(void* data) noexcept {
  delete static_cast<Source*>(data);
  source_exists.store(false);
}

obs_properties_t* properties(void*) {
  auto* result = obs_properties_create();
  obs_properties_add_text(result, "information", obs_module_text("Information"), OBS_TEXT_INFO);
  return result;
}
}  // namespace

bool obs_module_load(void) {
  obs_source_info info{};
  info.id = "soundfork_audio";
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
  info.get_name = source_name;
  info.create = create_source;
  info.destroy = destroy_source;
  info.get_properties = properties;
  info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
  obs_register_source(&info);
  return true;
}

MODULE_EXPORT const char* obs_module_description(void) {
  return "Direct SoundFork ASIO capture without a virtual audio endpoint";
}
