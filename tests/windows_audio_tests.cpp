#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include "audio-bridge/windows_audio_driver.hpp"
#include "audio-bridge/windows_output_settings.hpp"
#include "audio-bridge/proxy_driver.hpp"
#include "capture-backend/shared_ring.hpp"

namespace {
void require(bool ok, const char* reason) { if(!ok) throw std::runtime_error(reason); }
template<class Predicate> void await(Predicate predicate) {
  const auto until = GetTickCount64() + 5000;
  while(!predicate()) { require(GetTickCount64() < until, "render deadline"); Sleep(1); }
}
float sample(UINT64 frame) { return (static_cast<float>(frame % 1000) - 500.0F) / 1000.0F; }
struct MockDevice final : soundfork::IWindowsRenderDevice {
  HANDLE event = nullptr;
  std::array<float, 1024> output{};
  std::atomic<UINT64> delivered{0};
  std::atomic<int> errors{0};
  std::atomic<bool> fail{false}, open_fail{false};
  HRESULT open(const std::wstring& endpoint, HANDLE signal) override {
    if(endpoint != L"test-device") return E_INVALIDARG;
    event = signal; return open_fail ? E_FAIL : S_OK;
  }
  void close() noexcept override { event = nullptr; }
  UINT32 capacity() const noexcept override { return 512; }
  HRESULT start() noexcept override { SetEvent(event); return S_OK; }
  HRESULT stop() noexcept override { return S_OK; }
  HRESULT reset() noexcept override { delivered = 0; return S_OK; }
  HRESULT acquire(UINT32* frames, float** data) noexcept override {
    if(fail) return E_FAIL;
    *frames = delivered.load() % 3 == 0 ? 96 : 320; *data = output.data(); return S_OK;
  }
  HRESULT release(UINT32 frames) noexcept override {
    const auto base = delivered.load();
    for(UINT32 i = 0; i < frames; ++i)
      if(output[i*2] != sample(base+i) || output[i*2+1] != -sample(base+i)) ++errors;
    delivered += frames; SetEvent(event); return S_OK;
  }
  HRESULT position(UINT64* frames, UINT64* nanoseconds) noexcept override {
    *frames = delivered.load(); *nanoseconds = *frames * 1000000000ULL / 48000; return S_OK;
  }
};
ASIOBufferInfo* output = nullptr;
long block_size = 0;
UINT64 generated = 0;
std::atomic<int> resets{0}, calls{0};
void fill(long index, ASIOBool) {
  auto* left = static_cast<float*>(output[0].buffers[index]);
  auto* right = static_cast<float*>(output[1].buffers[index]);
  for(long frame = 0; frame < block_size; ++frame) {
    left[frame] = sample(generated + static_cast<UINT64>(frame)); right[frame] = -left[frame];
  }
  generated += static_cast<UINT64>(block_size); ++calls;
}
void rate(ASIOSampleRate) {}
long message(long selector, long, void*, double*) {
  if(selector == kAsioResetRequest) ++resets;
  return selector == kAsioSupportsTimeInfo ? 1 : 0;
}
ASIOTime* timed(ASIOTime* time, long index, ASIOBool direct) { fill(index,direct); return time; }
struct Factory : soundfork::IBackendFactory {
  MockDevice* mock = nullptr;
  HRESULT create(IASIO** driver) noexcept override {
    try {
      auto device = std::make_unique<MockDevice>(); mock = device.get();
      *driver = new soundfork::WindowsAudioDriver(L"test-device", std::move(device)); return S_OK;
    } catch(...) { return E_OUTOFMEMORY; }
  }
};
void test(long frames, bool use_time) {
  Factory factory;
  const auto mapping = L"Local\\SoundFork.WindowsAudioTest." + std::to_wstring(GetCurrentProcessId());
  auto* proxy = new soundfork::ProxyDriver(factory, mapping.c_str());
  require(proxy->init(nullptr) == ASIOTrue, "virtual init");
  require(proxy->canSampleRate(44100) == ASE_NoClock, "unsupported rate accepted");
  ASIOChannelInfo info{};
  require(proxy->getChannelInfo(&info) == ASE_OK && info.type == ASIOSTFloat32LSB, "virtual sample type");
  ASIOBufferInfo buffers[2]{}; buffers[1].channelNum = 1; output = buffers; block_size = frames;
  ASIOCallbacks callbacks{fill, rate, message, use_time ? timed : nullptr};
  require(proxy->createBuffers(buffers, 2, frames, &callbacks) == ASE_OK, "virtual buffers");
  soundfork::capture::SharedRingReader reader;
  require(reader.open(mapping.c_str()), "virtual capture attach");
  calls = 0; generated = 0;
  require(proxy->start() == ASE_OK, "virtual start");
  const auto needed = static_cast<UINT64>(frames) * (soundfork::capture::kCapacityBlocks + 8);
  await([&] { return factory.mock->delivered >= needed; });
  require(proxy->stop() == ASE_OK, "virtual stop");
  require(factory.mock->errors == 0 && calls > 0, "Windows output waveform discontinuity");
  auto capture = std::make_unique<std::array<std::byte, 16384>>(); std::uint32_t count = 0;
  require(reader.read_planar(capture->data(), capture->size(), &count) && count == static_cast<std::uint32_t>(frames),
      "virtual backend did not reach existing capture path");
  require(reader.stats().dropped_blocks > 0, "slow broadcast consumer was not isolated");
  ASIOSamples position{}; ASIOTimeStamp timestamp{};
  require(proxy->getSamplePosition(&position, &timestamp) == ASE_OK && position.lo > 0, "device-clock snapshot");
  generated = 0; calls = 0;
  require(proxy->start() == ASE_OK, "virtual restart");
  await([&] { return calls > 3; });
  factory.mock->fail = true;
  await([&] { return resets > 0; });
  require(proxy->stop() == ASE_OK, "stop after device invalidation");
  require(proxy->disposeBuffers() == ASE_OK, "virtual dispose");
  reader.close(); proxy->Release(); resets = 0;
}
void test_settings() {
  const auto path = L"Software\\SoundForkWindowsSettingsTest-" + std::to_wstring(GetCurrentProcessId());
  struct Scope {
    std::wstring path; HKEY key = nullptr;
    ~Scope() { RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr); if(key) RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
  } scope{path};
  require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
      &scope.key, nullptr) == ERROR_SUCCESS, "private settings root");
  require(RegOverridePredefKey(HKEY_LOCAL_MACHINE, scope.key) == ERROR_SUCCESS, "private registry override");
  std::wstring endpoint;
  require(soundfork::read_windows_output(&endpoint) == S_FALSE, "default is not vendor ASIO");
  require(soundfork::save_windows_output(L"test-device") == ERROR_SUCCESS &&
      soundfork::read_windows_output(&endpoint) == S_OK && endpoint == L"test-device", "Windows device persistence");
  require(soundfork::save_windows_output(std::wstring(L"bad\0id", 6)) == ERROR_INVALID_DATA, "embedded NUL accepted");
  soundfork::RegistryBackendFactory factory;
  IASIO* driver = nullptr; bool windows = false;
  require(factory.create_with_mode(&driver, &windows) == S_OK && windows && driver, "Windows backend factory selection");
  driver->Release(); driver = nullptr;
  require(soundfork::save_windows_output(L"") == ERROR_SUCCESS &&
      soundfork::read_windows_output(&endpoint) == S_FALSE, "vendor mode restore");
  require(FAILED(factory.create_with_mode(&driver, &windows)) && !windows && !driver,
      "missing vendor unexpectedly fell back to Windows");
}
}
int main() {
  try {
    test_settings();
    for(long frames : {64L, 256L, 2048L}) { test(frames,false); test(frames,true); }
    auto mock = std::make_unique<MockDevice>(); mock->open_fail = true;
    soundfork::WindowsAudioDriver driver(L"test-device", std::move(mock));
    require(driver.init(nullptr) == ASIOTrue, "failure fixture init");
    ASIOBufferInfo buffers[2]{}; buffers[1].channelNum = 1;
    ASIOCallbacks cb{fill,rate,message,timed};
    require(driver.createBuffers(buffers, 2, 63, &cb) == ASE_InvalidParameter, "invalid block accepted");
    require(driver.createBuffers(buffers, 2, 64, &cb) == ASE_HWMalfunction, "device open failure hidden");
    require(driver.disposeBuffers() == ASE_OK, "failed preparation cleanup");
    std::cout << "Virtual ASIO callbacks, partial periods, clock, restart, device failure and capture isolation passed\n";
    return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
