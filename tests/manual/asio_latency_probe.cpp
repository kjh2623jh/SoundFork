// Manual measurement only: no registration, device-setting changes or CTest entry.
// Measures host PCM-write QPC -> WASAPI loopback sample timestamp/packet arrival.
// This does NOT measure the DAC, speakers, touch-to-sound or physical loopback latency.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "audio-bridge/windows_audio_driver.hpp"
#include "audio-bridge/proxy_driver.hpp"
using Microsoft::WRL::ComPtr;
namespace {
std::string utf8(const std::wstring& text) {
  const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if(count <= 1) return {};
  std::string value(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, value.data(), count, nullptr, nullptr);
  value.pop_back(); return value;
}
void check(HRESULT result, const char* operation) {
  if(FAILED(result)) { std::cerr << operation << ": 0x" << std::hex << static_cast<unsigned long>(result) << std::dec << '\n';
    throw std::runtime_error(operation); }
}
UINT64 frequency = 0;
UINT64 now100ns() noexcept {
  LARGE_INTEGER value{}; QueryPerformanceCounter(&value);
  const auto ticks = static_cast<UINT64>(value.QuadPart);
  return ticks / frequency * 10000000ULL + ticks % frequency * 10000000ULL / frequency;
}
struct Marker { UINT64 sample_time = 0, arrival = 0; DWORD flags = 0; };
struct Context {
  ASIOBufferInfo buffers[2]{};
  ASIOSampleType type = ASIOSTFloat32LSB;
  long frames = 256;
  UINT64 generated = 0;
  std::array<UINT64, 64> sent{};
  std::size_t count = 0;
  std::atomic<bool> reset{false};
} context;
void write_sample(void* data, long frame, float value) noexcept {
  switch(context.type) {
    case ASIOSTFloat32LSB: static_cast<float*>(data)[frame] = value; break;
    case ASIOSTInt32LSB: static_cast<INT32*>(data)[frame] = static_cast<INT32>(value * 2147483647.0F); break;
    case ASIOSTInt16LSB: static_cast<INT16*>(data)[frame] = static_cast<INT16>(value * 32767.0F); break;
    default: break;
  }
}
void buffer_switch(long index, ASIOBool) {
  for(long frame = 0; frame < context.frames; ++frame) {
    const auto pos = context.generated + static_cast<UINT64>(frame);
    // 1 second warmup, then a 1 ms (-40 dBFS) cosine burst every half second.
    // Stop markers well before stopping the driver, so queued audio can drain normally.
    const bool pulse = pos >= 48000 && pos < 48000 + 20 * 24000 && (pos - 48000) % 24000 < 48;
    if(pulse && (pos - 48000) % 24000 == 0 && context.count < context.sent.size())
      context.sent[context.count++] = now100ns();
    const float value = pulse ? 0.01F * static_cast<float>(std::cos(
        6.283185307179586 * static_cast<double>((pos - 48000) % 48) / 48.0)) : 0.0F;
    write_sample(context.buffers[0].buffers[index], frame, value);
    write_sample(context.buffers[1].buffers[index], frame, value);
  }
  context.generated += static_cast<UINT64>(context.frames);
}
void sample_rate(ASIOSampleRate) {}
long asio_message(long selector, long, void*, double*) {
  if(selector == kAsioResetRequest) context.reset = true;
  return 0;
}
struct Capture {
  std::thread thread;
  std::atomic<bool> stop{false}, ready{false};
  HRESULT result = E_PENDING;
  std::vector<Marker> markers;
  UINT64 packets = 0, discontinuities = 0, timestamp_errors = 0;
  ~Capture() { stop = true; if(thread.joinable()) thread.join(); }
  void start(std::wstring id) { thread = std::thread([this, id = std::move(id)] { run(id); }); }
  void run(const std::wstring& id) noexcept {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WAVEFORMATEX* format = nullptr;
    ComPtr<IAudioClient> client;
    bool started = false;
    try {
      check(com, "capture COM");
      ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDevice> device;
      check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "capture enumerator");
      check(enumerator->GetDevice(id.c_str(), &device), "capture endpoint");
      check(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
          reinterpret_cast<void**>(client.GetAddressOf())), "capture activate");
      check(client->GetMixFormat(&format), "capture format");
      std::cout << "Loopback mix: rate=" << format->nSamplesPerSec << ", channels=" << format->nChannels
          << ", bits=" << format->wBitsPerSample << '\n';
      bool floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
      if(format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22)
        floating = IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
      if(!floating || format->wBitsPerSample != 32 || !format->nChannels || !format->nSamplesPerSec)
        throw std::runtime_error("probe currently requires Float32 loopback mix format");
      check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
          1000000, 0, format, nullptr), "capture initialize");
      ComPtr<IAudioCaptureClient> capture;
      check(client->GetService(IID_PPV_ARGS(&capture)), "capture service");
      markers.reserve(64);
      check(client->Start(), "capture start"); started = true;
      result = S_OK; ready = true;
      UINT64 quiet = format->nSamplesPerSec; // allow detection of a first unexpected signal, too
      while(!stop) {
        UINT32 pending = 0;
        check(capture->GetNextPacketSize(&pending), "capture pending");
        if(!pending) { Sleep(1); continue; }
        BYTE* bytes = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 position = 0, stamp = 0;
        check(capture->GetBuffer(&bytes, &frames, &flags, &position, &stamp), "capture buffer");
        const auto arrival = now100ns(); ++packets;
        if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
        if(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++timestamp_errors;
        for(UINT32 i = 0; i < frames; ++i) {
          float value = 0;
          if(!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && bytes)
            std::memcpy(&value, bytes + static_cast<std::size_t>(i) * format->nBlockAlign, sizeof(value));
          if(std::isfinite(value) && std::abs(value) > 0.002F) {
            if(quiet >= format->nSamplesPerSec / 100 && markers.size() < 64)
              markers.push_back({stamp + static_cast<UINT64>(i) * 10000000ULL / format->nSamplesPerSec, arrival, flags});
            quiet = 0;
          } else ++quiet;
        }
        check(capture->ReleaseBuffer(frames), "capture release");
      }
    } catch(...) { result = E_FAIL; ready = true; }
    if(started) (void)client->Stop();
    client.Reset();
    if(format) CoTaskMemFree(format);
    if(SUCCEEDED(com)) CoUninitialize();
  }
};
struct Factory : soundfork::IBackendFactory {
  std::wstring endpoint; soundfork::WindowsAudioDriver* windows = nullptr;
  HRESULT create(IASIO** driver) noexcept override {
    try { windows = new soundfork::WindowsAudioDriver(endpoint); *driver = windows; return S_OK; }
    catch(...) { *driver = nullptr; return E_OUTOFMEMORY; }
  }
  HRESULT create_with_mode(IASIO** driver, bool* mode) noexcept override { *mode = true; return create(driver); }
};
struct Driver {
  IASIO* value = nullptr; HMODULE module = nullptr;
  ~Driver() { if(value) { (void)value->stop(); (void)value->disposeBuffers(); value->Release(); } if(module) FreeLibrary(module); }
};
std::wstring device_id(IMMDevice* device) {
  LPWSTR id = nullptr; check(device->GetId(&id), "device ID");
  std::wstring result = id; CoTaskMemFree(id); return result;
}
std::wstring device_name(IMMDevice* device) {
  ComPtr<IPropertyStore> store; PROPVARIANT value; PropVariantInit(&value);
  std::wstring name;
  if(SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
      SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) name = value.pwszVal;
  PropVariantClear(&value); return name;
}
}
int wmain(int argc, wchar_t** argv) {
  const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if(FAILED(com)) return 1;
  struct ComEnd { ~ComEnd() { CoUninitialize(); } } com_end;
  try {
    LARGE_INTEGER freq{}; QueryPerformanceFrequency(&freq); frequency = static_cast<UINT64>(freq.QuadPart);
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "enumerator");
    if(argc == 2 && std::wstring(argv[1]) == L"--list") {
      for(const auto flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> devices;
        check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, &devices), "devices");
        UINT count = 0; check(devices->GetCount(&count), "device count");
        for(UINT i = 0; i < count; ++i) {
          ComPtr<IMMDevice> device; check(devices->Item(i, &device), "device"); DWORD state = 0; (void)device->GetState(&state);
          std::cout << (flow == eRender ? "render" : "capture") << " state=" << state << " "
              << utf8(device_name(device.Get())) << "\n  " << utf8(device_id(device.Get())) << '\n';
        }
      }
      return 0;
    }
    if(argc < 4) { std::cerr << "--list OR <soundfork|dll|registered> <endpoint-id|@default> <frames> [DLL-path CLSID | registered-CLSID]\n"; return 2; }
    const std::wstring mode = argv[1]; std::wstring endpoint = argv[2]; context.frames = std::wcstol(argv[3], nullptr, 10);
    if(context.frames < 64 || context.frames > 2048) throw std::runtime_error("block out of range");
    ComPtr<IMMDevice> device;
    if(endpoint == L"@default") check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "default device");
    else check(enumerator->GetDevice(endpoint.c_str(), &device), "device");
    endpoint = device_id(device.Get());
    std::cout << "Endpoint: " << utf8(device_name(device.Get())) << "\nID: " << utf8(endpoint) << '\n';
    Factory factory; factory.endpoint = endpoint;
    Driver driver;
    if(mode == L"soundfork") {
      const auto mapping = L"Local\\SoundFork.LatencyProbe." + std::to_wstring(GetCurrentProcessId());
      driver.value = new soundfork::ProxyDriver(factory, mapping.c_str());
    } else {
      CLSID clsid{};
      if(mode == L"dll" && argc == 6) {
        check(CLSIDFromString(argv[5], &clsid), "DLL CLSID");
        driver.module = LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!driver.module) throw std::runtime_error("LoadLibraryEx failed");
        const auto entry = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(driver.module, "DllGetClassObject"));
        if(!entry) throw std::runtime_error("DllGetClassObject absent");
        ComPtr<IClassFactory> class_factory;
        check(entry(clsid, IID_PPV_ARGS(&class_factory)), "DLL factory");
        check(class_factory->CreateInstance(nullptr, clsid, reinterpret_cast<void**>(&driver.value)), "DLL create");
      } else if(mode == L"registered" && argc == 5) {
        check(CLSIDFromString(argv[4], &clsid), "registered CLSID");
        check(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&driver.value)), "registered ASIO create");
      } else throw std::runtime_error("invalid mode/arguments");
    }
    if(driver.value->init(nullptr) != ASIOTrue) throw std::runtime_error("ASIO init failed");
    if(driver.value->setSampleRate(48000) != ASE_OK) throw std::runtime_error("48 kHz rejected");
    ASIOChannelInfo info{};
    if(driver.value->getChannelInfo(&info) != ASE_OK) throw std::runtime_error("channel info failed");
    context.type = info.type;
    if(context.type != ASIOSTFloat32LSB && context.type != ASIOSTInt32LSB && context.type != ASIOSTInt16LSB)
      throw std::runtime_error("probe sample format unsupported");
    context.buffers[1].channelNum = 1;
    ASIOCallbacks callbacks{buffer_switch, sample_rate, asio_message, nullptr};
    if(driver.value->createBuffers(context.buffers, 2, context.frames, &callbacks) != ASE_OK)
      throw std::runtime_error("ASIO buffers failed");
    if(factory.windows) {
      const auto output = factory.windows->output_info();
      std::cout << "Engine period=" << output.period << ", target=" << output.queue_target << ", capacity=" << output.capacity
          << ", low-latency HRESULT=0x" << std::hex << static_cast<unsigned long>(output.low_latency_result) << std::dec << '\n';
    }
    Capture capture; capture.start(endpoint);
    for(int i = 0; !capture.ready && i < 5000; ++i) Sleep(1);
    if(!capture.ready || FAILED(capture.result)) throw std::runtime_error("loopback unavailable");
    Sleep(200); // baseline before any test samples
    if(driver.value->start() != ASE_OK) throw std::runtime_error("ASIO start failed");
    Sleep(12000);
    (void)driver.value->stop(); Sleep(300); capture.stop = true; capture.thread.join();
    std::cout << "sent=" << context.count << ", detected=" << capture.markers.size() << ", packets=" << capture.packets
        << ", discontinuity_packets=" << capture.discontinuities << ", timestamp_errors=" << capture.timestamp_errors << '\n';
    if(capture.markers.size() != context.count) {
      for(std::size_t i = 0; i < context.count; ++i) std::cout << "unmatched_sent," << i << ',' << context.sent[i] << '\n';
      for(std::size_t i = 0; i < capture.markers.size(); ++i)
        std::cout << "unmatched_capture," << i << ',' << capture.markers[i].sample_time << ',' << capture.markers[i].arrival << '\n';
    }
    if(FAILED(capture.result) || context.reset || !context.count || capture.markers.size() != context.count)
      throw std::runtime_error("invalid capture/reset/count mismatch: do not infer latency");
    std::vector<double> delays, arrivals;
    std::cout << "marker,loopback_sample_ms,packet_arrival_ms,flags\n";
    for(std::size_t i = 0; i < context.count; ++i) {
      const auto& marker = capture.markers[i];
      const double sample = (static_cast<double>(marker.sample_time) - static_cast<double>(context.sent[i])) / 10000.0;
      const double arrival = (static_cast<double>(marker.arrival) - static_cast<double>(context.sent[i])) / 10000.0;
      std::cout << i << ',' << sample << ',' << arrival << ',' << marker.flags << '\n';
      if((marker.flags & (AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR | AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) ||
          sample < 0.0 || sample > 500.0 || arrival < 0.0 || arrival > 500.0)
        throw std::runtime_error("invalid/ambiguous marker timing");
      delays.push_back(sample);
      arrivals.push_back(arrival);
    }
    std::sort(delays.begin(), delays.end());
    std::sort(arrivals.begin(), arrivals.end());
    std::cout << "SOFTWARE_LOOPBACK_ONLY min=" << delays.front() << ", median=" << delays[delays.size()/2]
        << ", max=" << delays.back() << " ms; not physical output latency\n";
    std::cout << "CAPTURE_ARRIVAL_ONLY min=" << arrivals.front() << ", median=" << arrivals[arrivals.size()/2]
        << ", max=" << arrivals.back() << " ms; includes capture delivery/batching\n";
    return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
