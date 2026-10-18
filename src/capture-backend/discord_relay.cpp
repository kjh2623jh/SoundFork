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
#include <cwctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "capture-backend/shared_ring.hpp"
#include "capture-backend/relay_control.hpp"

namespace {

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
    std::wstring* selected_name) {
  ComPtr<IMMDevice> result;
  if (requested.empty()) {
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

  const std::wstring needle = lowercase(std::wstring(requested));
  for (const EndpointRecord& endpoint : endpoints) {
    if (lowercase(endpoint.name).find(needle) != std::wstring::npos ||
        lowercase(endpoint.id).find(needle) != std::wstring::npos) {
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
}

bool succeeded_asio_style(HRESULT result, const wchar_t* operation) {
  if (SUCCEEDED(result)) {
    return true;
  }
  std::wcerr << operation << L" failed: 0x" << std::hex
             << static_cast<unsigned long>(result) << std::dec << L'\n';
  return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  bool list_endpoints = false;
  bool system_share = false;
  bool persistent = false;
  bool stop_requested = false;
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
    } else if (argument == L"--endpoint" && index + 1 < argc) {
      endpoint_name = argv[++index];
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
    stop_event.value = CreateEventW(
        nullptr, TRUE, FALSE, soundfork::capture::kRelayStopEventName);
    if (stop_event.value == nullptr) {
      std::wcerr << L"Cannot create the relay stop event\n";
      return 10;
    }

    std::wstring selected_name;
    ComPtr<IMMDevice> endpoint = select_endpoint(
        enumerator.Get(), endpoints, endpoint_name, &selected_name);
    if (endpoint == nullptr) {
      std::wcerr << L"Requested render endpoint was not found\n";
      return 4;
    }

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
        reader.open();
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
    if (format.sample_rate != 48000 || format.channel_count != 2 ||
        format.bytes_per_sample != 4) {
      std::wcerr << L"Unsupported SoundFork capture format\n";
      return 7;
    }

    ComPtr<IAudioClient> audio_client;
    result = endpoint->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(audio_client.GetAddressOf()));
    if (!succeeded_asio_style(result, L"IAudioClient activation")) {
      return 8;
    }

    WAVEFORMATEXTENSIBLE wave_format{};
    wave_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave_format.Format.nChannels = 2;
    wave_format.Format.nSamplesPerSec = 48000;
    wave_format.Format.wBitsPerSample = 32;
    wave_format.Format.nBlockAlign = 8;
    wave_format.Format.nAvgBytesPerSec = 48000 * 8;
    wave_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
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
    if (!succeeded_asio_style(result, L"IAudioClient initialization")) {
      return 8;
    }

    if (!system_share) {
      ComPtr<IAudioViewManagerService> view_manager;
      result = audio_client->GetService(IID_PPV_ARGS(&view_manager));
      if (!succeeded_asio_style(result, L"IAudioViewManagerService query") ||
          !succeeded_asio_style(
              view_manager->SetAudioStreamWindow(target_window),
              L"Audio stream/window association")) {
        std::wcerr
            << L"This Windows build cannot associate relay audio with DJMAX.\n";
        return 9;
      }
    }

    HANDLE audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (audio_event == nullptr ||
        !succeeded_asio_style(
            audio_client->SetEventHandle(audio_event), L"SetEventHandle")) {
      if (audio_event != nullptr) {
        CloseHandle(audio_event);
      }
      return 8;
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
    if (!succeeded_asio_style(result, L"Initial render buffer")) {
      CloseHandle(audio_event);
      return 8;
    }

    std::wcout << L"Endpoint: " << selected_name << L'\n';
    if (system_share) {
      std::wcout << L"Mode: entire-screen/system audio\n";
    } else {
      std::wcout << L"Associated window: " << matched_title << L'\n';
    }
    std::wcout << L"Format: 48000 Hz, stereo Int32LSB\n";
    result = audio_client->Start();
    if (!succeeded_asio_style(result, L"IAudioClient start")) {
      CloseHandle(audio_event);
      return 8;
    }

    std::array<std::int32_t,
               soundfork::capture::kMaxFramesPerBlock *
                   soundfork::capture::kChannelCount>
        source{};
    std::uint32_t source_frames = 0;
    std::uint32_t source_offset = 0;
    std::uint64_t rendered_frames = 0;
    std::uint64_t silent_frames = 0;
    const ULONGLONG run_deadline =
        GetTickCount64() + static_cast<ULONGLONG>(duration_seconds) * 1000;
    const HANDLE wait_handles[] = {audio_event, stop_event.value};
    while (persistent || GetTickCount64() < run_deadline) {
      const DWORD wait_result = WaitForMultipleObjects(
          static_cast<DWORD>(std::size(wait_handles)),
          wait_handles,
          FALSE,
          200);
      if (wait_result == WAIT_OBJECT_0 + 1) {
        break;
      }
      if (wait_result != WAIT_OBJECT_0) {
        continue;
      }
      UINT32 padding = 0;
      if (FAILED(audio_client->GetCurrentPadding(&padding)) ||
          padding >= endpoint_buffer_frames) {
        continue;
      }
      const UINT32 available = endpoint_buffer_frames - padding;
      BYTE* raw_destination = nullptr;
      if (FAILED(render_client->GetBuffer(available, &raw_destination))) {
        continue;
      }
      auto* destination = reinterpret_cast<std::int32_t*>(raw_destination);
      bool all_silent = true;
      for (UINT32 frame = 0; frame < available; ++frame) {
        if (source_offset >= source_frames) {
          source_offset = 0;
          source_frames = 0;
          reader.read_int32_stereo(
              source.data(), source.size(), &source_frames);
        }
        if (source_offset < source_frames) {
          destination[frame * 2] = source[source_offset * 2];
          destination[frame * 2 + 1] = source[source_offset * 2 + 1];
          ++source_offset;
          all_silent = false;
        } else {
          destination[frame * 2] = 0;
          destination[frame * 2 + 1] = 0;
          ++silent_frames;
        }
      }
      render_client->ReleaseBuffer(
          available, all_silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
      rendered_frames += available;
    }

    audio_client->Stop();
    CloseHandle(audio_event);
    const auto stats = reader.stats();
    std::wcout << L"Rendered frames: " << rendered_frames
               << L"\nUnderflow/silent frames: " << silent_frames
               << L"\nCapture dropped blocks: " << stats.dropped_blocks << L'\n';
  }
  return exit_code;
}
