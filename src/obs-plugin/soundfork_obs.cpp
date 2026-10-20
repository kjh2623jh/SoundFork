#include <windows.h>
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

void capture(Source& state) {
  using namespace soundfork::capture;
  HubClient hub(HubRole::obs);
  SharedRingReader reader;
  std::unique_ptr<RelayAudioSource> audio;
  std::unique_ptr<RelayClockController> clock;
  constexpr std::uint32_t frames = 480;
  std::array<std::int32_t, frames * 2> samples{};
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
  soundfork::obs::AudioTimeline timeline;
  std::uint64_t next_probe = 0;
  std::uint64_t packets = 0;
  std::uint64_t silent_frames = 0;
  std::uint64_t discarded = 0;
  std::uint64_t resyncs = 0;
  std::uint64_t underruns = 0;
  std::uint64_t maximum_queue = 0;
  double maximum_ppm = 0;
  bool prebuffered = false;
  while (WaitForSingleObject(state.stop, 0) == WAIT_TIMEOUT) {
    const auto probe_now = os_gettime_ns();
    if (probe_now >= next_probe) {
      next_probe = probe_now + 250000000;
      if (reader.is_open() && !reader.stream_active()) {
        reader.close();
        audio.reset();
        clock.reset();
        prebuffered = false;
      }
      if (!reader.is_open()) {
        if (reader.open(hub.mapping_name())) {
          const auto format = reader.format();
          if (format.sample_rate != 48000 || format.channel_count != 2) {
            reader.close();
          } else {
            audio = std::make_unique<RelayAudioSource>(format);
            clock = std::make_unique<RelayClockController>(
                format.sample_rate, format.frames_per_block);
            blog(LOG_INFO, "[SoundFork] Attached: %u Hz, %u frames/block",
                 format.sample_rate, format.frames_per_block);
          }
        }
      }
    }
    // A timer signal is not a sample clock: service every elapsed packet
    // deadline, even if Windows coalesced or delayed wake-ups.
    const auto now = os_gettime_ns();
    const auto batch = timeline.plan(now);
    if (batch.resynchronized) {
      if (reader.is_open() && audio && clock) {
        discarded += reader.discard_oldest(4);
        audio->reset();
        clock->reset();
        prebuffered = false;
        ++resyncs;
      }
    }
    for (std::uint32_t packet_index = 0; packet_index < batch.packets; ++packet_index) {
      if (WaitForSingleObject(state.stop, 0) != WAIT_TIMEOUT) break;
      samples.fill(0);
      if (reader.is_open() && audio && clock) {
        auto queued = audio->buffered_frames(reader);
        maximum_queue = (std::max)(maximum_queue, queued);
        if (queued > static_cast<std::uint64_t>(reader.format().frames_per_block) * 48) {
          discarded += reader.discard_oldest(4);
          audio->reset();
          clock->reset();
          prebuffered = false;
          queued = audio->buffered_frames(reader);
          ++resyncs;
        }
        if (!prebuffered && queued >= clock->target_frames() + frames) {
          prebuffered = true;
        }
        if (prebuffered) {
          const double ratio = clock->update(queued > frames ? queued - frames : 0, frames);
          maximum_ppm = (std::max)(maximum_ppm, std::abs(clock->correction_ppm()));
          const auto before = audio->metrics().silent_frames;
          audio->render(reader, samples.data(), frames, ratio);
          silent_frames += audio->metrics().silent_frames - before;
          if (audio->metrics().silent_frames != before) ++underruns;
        }
      }
      if (!prebuffered) silent_frames += frames;
      obs_source_audio packet{};
      packet.data[0] = reinterpret_cast<const std::uint8_t*>(samples.data());
      packet.frames = frames;
      packet.speakers = SPEAKERS_STEREO;
      packet.format = AUDIO_FORMAT_32BIT;
      packet.samples_per_sec = 48000;
      packet.timestamp = timeline.timestamp();
      obs_source_output_audio(state.source, &packet);
      timeline.advance();
      ++packets;
    }
    // Rearm relative to the absolute sample deadline, not relative to the last
    // wake. A one-shot timer cannot silently lose a count of audio periods.
    const auto after = os_gettime_ns();
    const auto remaining = timeline.timestamp() > after ? timeline.timestamp() - after : 100;
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>((remaining + 99) / 100);
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      blog(LOG_ERROR, "[SoundFork] Cannot arm capture timer");
      break;
    }
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
  }
  blog(LOG_INFO,
       "[SoundFork] packets=%llu silent_frames=%llu max_queue_frames=%llu "
       "discarded_blocks=%llu resyncs=%llu underruns=%llu "
       "hub_output_drops=%llu max_correction_ppm=%.2f",
       static_cast<unsigned long long>(packets),
       static_cast<unsigned long long>(silent_frames),
       static_cast<unsigned long long>(maximum_queue),
       static_cast<unsigned long long>(discarded),
       static_cast<unsigned long long>(resyncs),
       static_cast<unsigned long long>(underruns),
       static_cast<unsigned long long>(reader.stats().dropped_blocks), maximum_ppm);
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
