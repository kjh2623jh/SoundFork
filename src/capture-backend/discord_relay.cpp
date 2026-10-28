#include <windows.h>
#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "capture-backend/shared_ring.hpp"
#include "capture-backend/hub_client.hpp"
#include "capture-backend/relay_clock.hpp"
#include "capture-backend/relay_control.hpp"
#include "capture-backend/relay_recovery.hpp"
#include "capture-backend/endpoint_selection.hpp"
#include "diagnostics/async_log.hpp"
#include "soundfork_version.hpp"

namespace {
namespace diagnostics = soundfork::diagnostics;

using Microsoft::WRL::ComPtr;

struct EndpointRecord {
  std::wstring name;
  std::wstring id;
  ComPtr<IMMDevice> device;
};

struct WindowSearch {
  std::wstring needle;
  HWND result = nullptr;
  std::wstring title;
};

struct ComUninitializer {
  ~ComUninitializer() {
    CoUninitialize();
  }
};

struct HandleCloser {
  HANDLE value = nullptr;
  ~HandleCloser() {
    if (value != nullptr) {
      CloseHandle(value);
    }
  }
};

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
    return static_cast<wchar_t>(std::towlower(character));
  });
  return value;
}

BOOL CALLBACK find_window_callback(HWND window, LPARAM parameter) {
  auto* search = reinterpret_cast<WindowSearch*>(parameter);
  if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) {
    return TRUE;
  }
  std::array<wchar_t, 512> title{};
  const int length = GetWindowTextW(
      window, title.data(), static_cast<int>(title.size()));
  if (length <= 0) {
    return TRUE;
  }
  std::wstring candidate(title.data(), static_cast<std::size_t>(length));
  if (lowercase(candidate).find(search->needle) == std::wstring::npos) {
    return TRUE;
  }
  search->result = window;
  search->title = std::move(candidate);
  return FALSE;
}

HWND find_window(std::wstring_view title_part, std::wstring* matched_title) {
  WindowSearch search{lowercase(std::wstring(title_part))};
  EnumWindows(&find_window_callback, reinterpret_cast<LPARAM>(&search));
  if (matched_title != nullptr) {
    *matched_title = search.title;
  }
  return search.result;
}

std::vector<EndpointRecord> enumerate_endpoints(IMMDeviceEnumerator* enumerator) {
  std::vector<EndpointRecord> endpoints;
  ComPtr<IMMDeviceCollection> collection;
  if (FAILED(enumerator->EnumAudioEndpoints(
          eRender, DEVICE_STATE_ACTIVE, &collection))) {
    return endpoints;
  }
  UINT count = 0;
  if (FAILED(collection->GetCount(&count))) {
    return endpoints;
  }
  for (UINT index = 0; index < count; ++index) {
    EndpointRecord record;
    if (FAILED(collection->Item(index, &record.device))) {
      continue;
    }
    LPWSTR id = nullptr;
    if (SUCCEEDED(record.device->GetId(&id)) && id != nullptr) {
      record.id = id;
      CoTaskMemFree(id);
    }
    ComPtr<IPropertyStore> properties;
    PROPVARIANT name;
    PropVariantInit(&name);
    if (SUCCEEDED(record.device->OpenPropertyStore(STGM_READ, &properties)) &&
        SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
        name.vt == VT_LPWSTR && name.pwszVal != nullptr) {
      record.name = name.pwszVal;
    }
    PropVariantClear(&name);
    if (record.name.empty()) {
      record.name = record.id;
    }
    endpoints.push_back(std::move(record));
  }
  return endpoints;
}

ComPtr<IMMDevice> select_endpoint(
    IMMDeviceEnumerator* enumerator,
    const std::vector<EndpointRecord>& endpoints,
    std::wstring_view requested,
    bool exact_id,
    std::wstring* selected_name) {
  ComPtr<IMMDevice> result;
  if (requested.empty() && !exact_id) {
    if (FAILED(enumerator->GetDefaultAudioEndpoint(
            eRender, eConsole, &result))) {
      return {};
    }
    LPWSTR selected_id = nullptr;
    if (SUCCEEDED(result->GetId(&selected_id)) && selected_id != nullptr) {
      for (const EndpointRecord& endpoint : endpoints) {
        if (endpoint.id == selected_id) {
          *selected_name = endpoint.name;
          break;
        }
      }
      CoTaskMemFree(selected_id);
    }
    return result;
  }

  for (const EndpointRecord& endpoint : endpoints) {
    if (soundfork::capture::endpoint_matches(endpoint.id, endpoint.name, requested, exact_id)) {
      result = endpoint.device;
      *selected_name = endpoint.name;
      return result;
    }
  }
  return {};
}

void print_usage() {
  std::wcout
      << L"SoundFork Discord WASAPI relay (experimental)\n"
      << L"  soundfork_discord_relay --list-endpoints\n"
      << L"  soundfork_discord_relay --stop\n"
      << L"  soundfork_discord_relay [--endpoint <name-part>] "
         L"--window-title <title-part> [--wait 60] [--duration 30]\n";
  std::wcout
      << L"  soundfork_discord_relay [--endpoint <name-part>] "
         L"--system-share [--wait 60] [--duration 30]\n";
  std::wcout
      << L"  soundfork_discord_relay [--endpoint <name-part>] "
         L"--system-share --persistent\n";
  std::wcout << L"  Use --endpoint-id <device-id> instead of --endpoint for an exact saved device (no fallback).\n";
}

bool succeeded_asio_style(HRESULT result, const wchar_t* operation) {
  if (SUCCEEDED(result)) {
    return true;
  }
  std::wcerr << operation << L" failed: 0x" << std::hex
             << static_cast<unsigned long>(result) << std::dec << L'\n';
  char phase[96]{};
  (void)WideCharToMultiByte(CP_UTF8, 0, operation, -1, phase, static_cast<int>(sizeof(phase)), nullptr, nullptr);
  diagnostics::log().write(diagnostics::Level::error, "wasapi_error",
      "operation=" + std::string(phase) + " hresult=" + std::to_string(static_cast<unsigned long>(result)));
  return false;
}

enum class SessionOutcome {
  complete,
  stopped,
  reconnect,
  fatal,
};

struct RelayMetrics {
  std::uint64_t rendered_frames = 0;
  std::uint64_t silent_frames = 0;
  std::uint64_t minimum_queue_frames =
      (std::numeric_limits<std::uint64_t>::max)();
  std::uint64_t maximum_queue_frames = 0;
  std::uint64_t resync_count = 0;
  std::uint64_t discarded_blocks = 0;
  std::uint64_t reconnect_count = 0;
  double maximum_correction_ppm = 0.0;
  std::uint64_t underflow_frames = 0, prebuffer_frames = 0, last_queue_frames = 0;
};

void log_metrics(const soundfork::capture::SharedRingReader& reader, const RelayMetrics& metrics,
                 std::uint64_t current_underflow = 0, std::uint64_t current_prebuffer = 0) {
  char detail[448]{};
  _snprintf_s(detail, sizeof(detail), _TRUNCATE,
      "rendered_frames=%llu queue_frames=%llu queue_min=%llu queue_max=%llu underflow_frames=%llu "
      "prebuffer_frames=%llu resyncs=%llu discarded_blocks=%llu session_reconnects=%llu "
      "hub_output_drops=%llu max_correction_ppm=%.2f",
      static_cast<unsigned long long>(metrics.rendered_frames),
      static_cast<unsigned long long>(metrics.last_queue_frames),
      static_cast<unsigned long long>(metrics.minimum_queue_frames == (std::numeric_limits<std::uint64_t>::max)()
          ? 0 : metrics.minimum_queue_frames),
      static_cast<unsigned long long>(metrics.maximum_queue_frames),
      static_cast<unsigned long long>(metrics.underflow_frames + current_underflow),
      static_cast<unsigned long long>(metrics.prebuffer_frames + current_prebuffer),
      static_cast<unsigned long long>(metrics.resync_count),
      static_cast<unsigned long long>(metrics.discarded_blocks),
      static_cast<unsigned long long>(metrics.reconnect_count),
      static_cast<unsigned long long>(reader.stats().dropped_blocks), metrics.maximum_correction_ppm);
  diagnostics::log().write(diagnostics::Level::info, "relay_metrics", detail);
}

SessionOutcome run_render_session(
    IMMDevice* endpoint,
    std::wstring_view selected_name,
    bool system_share,
    HWND target_window,
    std::wstring_view matched_title,
    bool persistent,
    ULONGLONG run_deadline,
    HANDLE stop_event,
    soundfork::capture::SharedRingReader* reader,
    RelayMetrics* metrics) {
  if (endpoint == nullptr || reader == nullptr || metrics == nullptr) {
    return SessionOutcome::fatal;
  }

  ComPtr<IAudioClient> audio_client;
  HRESULT result = endpoint->Activate(
      __uuidof(IAudioClient),
      CLSCTX_ALL,
      nullptr,
      reinterpret_cast<void**>(audio_client.GetAddressOf()));
  if (FAILED(result)) {
    succeeded_asio_style(result, L"IAudioClient activation");
    return soundfork::capture::recoverable_audio_error(result)
               ? SessionOutcome::reconnect
               : SessionOutcome::fatal;
  }

  WAVEFORMATEXTENSIBLE wave_format{};
  wave_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wave_format.Format.nChannels = 2;
  wave_format.Format.nSamplesPerSec = 48000;
  wave_format.Format.wBitsPerSample = 32;
  wave_format.Format.nBlockAlign = 8;
  wave_format.Format.nAvgBytesPerSec = 48000 * 8;
  wave_format.Format.cbSize =
      sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  wave_format.Samples.wValidBitsPerSample = 32;
  wave_format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  wave_format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

  constexpr DWORD kStreamFlags =
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST |
      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  result = audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      kStreamFlags,
      0,
      0,
      &wave_format.Format,
      nullptr);
  if (FAILED(result)) {
    succeeded_asio_style(result, L"IAudioClient initialization");
    return soundfork::capture::recoverable_audio_error(result)
               ? SessionOutcome::reconnect
               : SessionOutcome::fatal;
  }

  if (!system_share) {
    ComPtr<IAudioViewManagerService> view_manager;
    result = audio_client->GetService(IID_PPV_ARGS(&view_manager));
    if (SUCCEEDED(result)) {
      result = view_manager->SetAudioStreamWindow(target_window);
    }
    if (FAILED(result)) {
      std::wcerr
          << L"This Windows build cannot associate relay audio with the "
             L"selected window.\n";
      return soundfork::capture::recoverable_audio_error(result)
                 ? SessionOutcome::reconnect
                 : SessionOutcome::fatal;
    }
  }

  HandleCloser audio_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  if (audio_event.value == nullptr) {
    return SessionOutcome::fatal;
  }
  result = audio_client->SetEventHandle(audio_event.value);
  if (FAILED(result)) {
    succeeded_asio_style(result, L"SetEventHandle");
    return soundfork::capture::recoverable_audio_error(result)
               ? SessionOutcome::reconnect
               : SessionOutcome::fatal;
  }

  ComPtr<IAudioRenderClient> render_client;
  UINT32 endpoint_buffer_frames = 0;
  result = audio_client->GetBufferSize(&endpoint_buffer_frames);
  if (SUCCEEDED(result)) {
    result = audio_client->GetService(IID_PPV_ARGS(&render_client));
  }
  BYTE* initial_buffer = nullptr;
  if (SUCCEEDED(result)) {
    result = render_client->GetBuffer(endpoint_buffer_frames, &initial_buffer);
  }
  if (SUCCEEDED(result)) {
    result = render_client->ReleaseBuffer(
        endpoint_buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
  }
  if (FAILED(result)) {
    succeeded_asio_style(result, L"Initial render buffer");
    return soundfork::capture::recoverable_audio_error(result)
               ? SessionOutcome::reconnect
               : SessionOutcome::fatal;
  }

  std::wcout << L"Endpoint: " << selected_name << L'\n';
  if (system_share) {
    std::wcout << L"Mode: entire-screen/system audio\n";
  } else {
    std::wcout << L"Associated window: " << matched_title << L'\n';
  }
  std::wcout << L"Source: " << soundfork::capture::sample_format_name(reader->format().sample_format)
             << L"; WASAPI render: 48000 Hz, stereo Int32LSB\n";
  result = audio_client->Start();
  if (FAILED(result)) {
    succeeded_asio_style(result, L"IAudioClient start");
    return soundfork::capture::recoverable_audio_error(result)
               ? SessionOutcome::reconnect
               : SessionOutcome::fatal;
  }

  const auto format = reader->format();
  diagnostics::log().write(diagnostics::Level::info, "render_session_start",
      "rate=48000 channels=2 frames_per_block=" + std::to_string(format.frames_per_block) +
      " endpoint_buffer_frames=" + std::to_string(endpoint_buffer_frames));
  auto source = std::make_unique<soundfork::capture::RelayAudioSource>(format);
  soundfork::capture::RelayClockController clock(
      format.sample_rate, format.frames_per_block);
  bool prebuffered = false;
  std::uint64_t prebuffer_silent_frames = 0;
  const HANDLE wait_handles[] = {audio_event.value, stop_event};
  SessionOutcome outcome = SessionOutcome::complete;
  const char* reason = "deadline";
  ULONGLONG next_metrics_at = GetTickCount64() + 30000;
  ULONGLONG next_gap_log_at = 0;
  while (persistent || GetTickCount64() < run_deadline) {
    if (!reader->stream_active()) {
      reason = "source_inactive";
      outcome = SessionOutcome::reconnect;
      break;
    }
    const DWORD wait_result = WaitForMultipleObjects(
        static_cast<DWORD>(std::size(wait_handles)),
        wait_handles,
        FALSE,
        200);
    if (wait_result == WAIT_OBJECT_0 + 1) {
      reason = "stop_requested";
      outcome = SessionOutcome::stopped;
      break;
    }
    if (wait_result == WAIT_TIMEOUT) {
      UINT32 ignored_padding = 0;
      result = audio_client->GetCurrentPadding(&ignored_padding);
      if (FAILED(result)) {
        outcome = soundfork::capture::recoverable_audio_error(result)
                      ? SessionOutcome::reconnect
                      : SessionOutcome::fatal;
        break;
      }
      continue;
    }
    if (wait_result != WAIT_OBJECT_0) {
      reason = "audio_wait_error";
      outcome = SessionOutcome::reconnect;
      break;
    }

    UINT32 padding = 0;
    result = audio_client->GetCurrentPadding(&padding);
    if (FAILED(result)) {
      outcome = soundfork::capture::recoverable_audio_error(result)
                    ? SessionOutcome::reconnect
                    : SessionOutcome::fatal;
      break;
    }
    if (padding >= endpoint_buffer_frames) {
      continue;
    }
    const UINT32 available = endpoint_buffer_frames - padding;
    BYTE* raw_destination = nullptr;
    result = render_client->GetBuffer(available, &raw_destination);
    if (FAILED(result)) {
      outcome = soundfork::capture::recoverable_audio_error(result)
                    ? SessionOutcome::reconnect
                    : SessionOutcome::fatal;
      break;
    }

    auto* destination = reinterpret_cast<std::int32_t*>(raw_destination);
    const auto underflow_before = source->metrics().silent_frames;
    const auto resync_before = metrics->resync_count;
    std::uint64_t queue_frames = source->buffered_frames(*reader);
    if (queue_frames >
        static_cast<std::uint64_t>(format.frames_per_block) * 48U) {
      metrics->discarded_blocks += reader->discard_oldest(4);
      source->reset();
      clock.reset();
      ++metrics->resync_count;
      queue_frames = source->buffered_frames(*reader);
    }
    metrics->minimum_queue_frames =
        (std::min)(metrics->minimum_queue_frames, queue_frames);
    metrics->maximum_queue_frames =
        (std::max)(metrics->maximum_queue_frames, queue_frames);
    metrics->last_queue_frames = queue_frames;
    if (!prebuffered && queue_frames >= clock.target_frames()) {
      prebuffered = true;
    }

    bool any_audio = false;
    if (prebuffered) {
      const double ratio = clock.update(queue_frames, available);
      metrics->maximum_correction_ppm = (std::max)(
          metrics->maximum_correction_ppm, std::abs(clock.correction_ppm()));
      any_audio = source->render(*reader, destination, available, ratio);
    } else {
      std::fill_n(destination, static_cast<std::size_t>(available) * 2, 0);
      prebuffer_silent_frames += available;
    }
    result = render_client->ReleaseBuffer(
        available, any_audio ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(result)) {
      outcome = soundfork::capture::recoverable_audio_error(result)
                    ? SessionOutcome::reconnect
                    : SessionOutcome::fatal;
      break;
    }
    metrics->rendered_frames += available;
    const auto now = GetTickCount64();
    if ((source->metrics().silent_frames != underflow_before || metrics->resync_count != resync_before) &&
        now >= next_gap_log_at) {
      next_gap_log_at = now + 1000;
      diagnostics::log().write(diagnostics::Level::warning, "relay_audio_gap",
          "underflow_frames=" + std::to_string(metrics->underflow_frames + source->metrics().silent_frames) +
          " resyncs=" + std::to_string(metrics->resync_count));
    }
    if (GetTickCount64() >= next_metrics_at) {
      next_metrics_at = GetTickCount64() + 30000;
      log_metrics(*reader, *metrics, source->metrics().silent_frames, prebuffer_silent_frames);
    }
  }

  audio_client->Stop();
  metrics->silent_frames +=
      prebuffer_silent_frames + source->metrics().silent_frames;
  metrics->underflow_frames += source->metrics().silent_frames;
  metrics->prebuffer_frames += prebuffer_silent_frames;
  diagnostics::log().write(FAILED(result) ? diagnostics::Level::warning : diagnostics::Level::info,
      "render_session_end", "outcome=" + std::to_string(static_cast<int>(outcome)) +
      " reason=" + (FAILED(result) ? "wasapi_error" : reason) +
      " hresult=" + std::to_string(static_cast<unsigned long>(result)));
  log_metrics(*reader, *metrics);
  return outcome;
}

}  // namespace

int relay_main(int argc, wchar_t** argv) {
  bool list_endpoints = false;
  bool system_share = false;
  bool persistent = false;
  bool stop_requested = false;
  bool endpoint_specified = false;
  bool exact_endpoint_id = false;
  std::wstring endpoint_name;
  std::wstring window_title;
  long wait_seconds = 60;
  long duration_seconds = 30;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--list-endpoints") {
      list_endpoints = true;
    } else if (argument == L"--system-share") {
      system_share = true;
    } else if (argument == L"--persistent") {
      persistent = true;
    } else if (argument == L"--stop") {
      stop_requested = true;
    } else if ((argument == L"--endpoint" || argument == L"--endpoint-id") && index + 1 < argc) {
      if (endpoint_specified) { print_usage(); return 2; }
      endpoint_specified = true;
      exact_endpoint_id = argument == L"--endpoint-id";
      endpoint_name = argv[++index];
      if (endpoint_name.empty()) { print_usage(); return 2; }
    } else if (argument == L"--window-title" && index + 1 < argc) {
      window_title = argv[++index];
    } else if (argument == L"--wait" && index + 1 < argc) {
      wait_seconds = std::wcstol(argv[++index], nullptr, 10);
    } else if (argument == L"--duration" && index + 1 < argc) {
      duration_seconds = std::wcstol(argv[++index], nullptr, 10);
    } else {
      print_usage();
      return 2;
    }
  }
  if ((!list_endpoints && !stop_requested &&
       (system_share == !window_title.empty())) ||
      wait_seconds < 0 ||
      duration_seconds <= 0) {
    print_usage();
    return 2;
  }

  if (stop_requested) {
    HandleCloser stop_event{OpenEventW(
        EVENT_MODIFY_STATE,
        FALSE,
        soundfork::capture::kRelayStopEventName)};
    if (stop_event.value == nullptr) {
      std::wcerr << L"No SoundFork Discord relay is running\n";
      return 11;
    }
    return SetEvent(stop_event.value) != FALSE ? 0 : 11;
  }

  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com_result)) {
    std::wcerr << L"COM initialization failed: 0x" << std::hex
               << static_cast<unsigned long>(com_result) << L'\n';
    return 3;
  }
  ComUninitializer com_uninitializer;

  HandleCloser singleton;
  HandleCloser stop_event;

  int exit_code = 0;
  {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (!succeeded_asio_style(result, L"MMDeviceEnumerator activation")) {
      return 3;
    }
    const std::vector<EndpointRecord> endpoints =
        enumerate_endpoints(enumerator.Get());
    if (list_endpoints) {
      for (const EndpointRecord& endpoint : endpoints) {
        std::wcout << endpoint.name << L"\n  " << endpoint.id << L'\n';
      }
      return 0;
    }

    singleton.value =
        CreateMutexW(nullptr, FALSE, soundfork::capture::kRelaySingletonName);
    if (singleton.value == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
      std::wcerr << L"Another SoundFork Discord relay is already running\n";
      return 10;
    }
    diagnostics::initialize_log(diagnostics::Component::relay, SOUNDFORK_VERSION_STRING);
    diagnostics::log().write(diagnostics::Level::info, "relay_start", "waiting_for_source=1");
    stop_event.value = CreateEventW(
        nullptr, TRUE, FALSE, soundfork::capture::kRelayStopEventName);
    if (stop_event.value == nullptr) {
      std::wcerr << L"Cannot create the relay stop event\n";
      return 10;
    }

    soundfork::capture::HubClient hub(soundfork::capture::HubRole::relay);
    soundfork::capture::SharedRingReader reader;
    HWND target_window = nullptr;
    std::wstring matched_title;
    const ULONGLONG wait_deadline =
        GetTickCount64() + static_cast<ULONGLONG>(wait_seconds) * 1000;
    while (persistent || GetTickCount64() <= wait_deadline) {
      if (WaitForSingleObject(stop_event.value, 0) == WAIT_OBJECT_0) {
        return 0;
      }
      if (!system_share && target_window == nullptr) {
        target_window = find_window(window_title, &matched_title);
      }
      if (!reader.is_open()) {
        hub.ensure_running();
        reader.open(hub.mapping_name());
      }
      if ((system_share || target_window != nullptr) && reader.is_open()) {
        break;
      }
      Sleep(50);
    }
    if (!system_share && target_window == nullptr) {
      std::wcerr << L"No visible window matching: " << window_title << L'\n';
      return 5;
    }
    if (!reader.is_open()) {
      std::wcerr << L"No active SoundFork capture stream was found\n";
      return 6;
    }
    const auto format = reader.format();
    diagnostics::log().write(diagnostics::Level::info, "source_connected");
    if (format.sample_rate != 48000 || format.channel_count != 2 ||
        sample_bytes(format.sample_format) == 0) {
      std::wcerr << L"Unsupported SoundFork capture format\n";
      return 7;
    }

    const ULONGLONG run_deadline =
        GetTickCount64() + static_cast<ULONGLONG>(duration_seconds) * 1000;
    RelayMetrics metrics;
    bool waiting_endpoint = false;
    while (persistent || GetTickCount64() < run_deadline) {
      if (WaitForSingleObject(stop_event.value, 0) == WAIT_OBJECT_0) {
        break;
      }
      if (!reader.stream_active()) {
        reader.close();
        hub.ensure_running();
        if (!reader.open(hub.mapping_name())) {
          if (WaitForSingleObject(stop_event.value, 50) == WAIT_OBJECT_0) break;
          continue;
        }
        const auto current_format = reader.format();
        if (current_format.sample_rate != 48000 ||
            current_format.channel_count != 2) {
          reader.close();
          if (WaitForSingleObject(stop_event.value, 250) == WAIT_OBJECT_0) break;
          continue;
        }
      }
      const std::vector<EndpointRecord> current_endpoints =
          enumerate_endpoints(enumerator.Get());
      std::wstring selected_name;
      ComPtr<IMMDevice> endpoint = select_endpoint(
          enumerator.Get(), current_endpoints, endpoint_name, exact_endpoint_id, &selected_name);
      if (endpoint == nullptr) {
        if (!waiting_endpoint) diagnostics::log().write(diagnostics::Level::warning, "endpoint_unavailable");
        waiting_endpoint = true;
        if (WaitForSingleObject(stop_event.value, 500) == WAIT_OBJECT_0) {
          break;
        }
        continue;
      }
      if (waiting_endpoint) diagnostics::log().write(diagnostics::Level::info, "endpoint_available");
      waiting_endpoint = false;
      const SessionOutcome outcome = run_render_session(
          endpoint.Get(),
          selected_name,
          system_share,
          target_window,
          matched_title,
          persistent,
          run_deadline,
          stop_event.value,
          &reader,
          &metrics);
      if (outcome == SessionOutcome::fatal) {
        return 8;
      }
      if (outcome == SessionOutcome::complete ||
          outcome == SessionOutcome::stopped) {
        break;
      }
      ++metrics.reconnect_count;
      diagnostics::log().write(diagnostics::Level::warning, "render_session_reconnect");
      std::wcerr << L"Audio endpoint disconnected; waiting to reconnect\n";
      if (WaitForSingleObject(stop_event.value, 500) == WAIT_OBJECT_0) {
        break;
      }
    }

    const auto stats = reader.stats();
    if (metrics.minimum_queue_frames ==
        (std::numeric_limits<std::uint64_t>::max)()) {
      metrics.minimum_queue_frames = 0;
    }
    std::wcout << L"Rendered frames: " << metrics.rendered_frames
               << L"\nUnderflow/silent frames: "
               << metrics.silent_frames
               << L"\nRelay queue frames (min/max): "
               << metrics.minimum_queue_frames << L"/"
               << metrics.maximum_queue_frames
               << L"\nMaximum clock correction: "
               << metrics.maximum_correction_ppm
               << L" ppm\nRelay resyncs: " << metrics.resync_count
               << L"\nEndpoint reconnects: " << metrics.reconnect_count
               << L"\nDiscarded stale blocks: " << metrics.discarded_blocks
               << L"\nCapture dropped blocks: " << stats.dropped_blocks << L'\n';
  }
  return exit_code;
}

int wmain(int argc, wchar_t** argv) {
  const int result = relay_main(argc, argv);
  diagnostics::log().write(result == 0 ? diagnostics::Level::info : diagnostics::Level::error,
      "relay_exit", "exit_code=" + std::to_string(result));
  diagnostics::log().stop();
  return result;
}
