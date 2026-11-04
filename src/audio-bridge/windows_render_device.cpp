#include "audio-bridge/windows_audio_driver.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <limits>
#include <algorithm>

namespace soundfork {
namespace {
using Microsoft::WRL::ComPtr;
class WasapiDevice final : public IWindowsRenderDevice {
 public:
  HRESULT open(const std::wstring& endpoint, HANDLE event) override {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&enumerator));
    // Exact selection only. Never silently switch to a default or broadcast endpoint.
    if (SUCCEEDED(result)) result = enumerator->GetDevice(endpoint.c_str(), &device);
    if (SUCCEEDED(result)) result = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
        nullptr, reinterpret_cast<void**>(client_.GetAddressOf()));
    WAVEFORMATEXTENSIBLE format{};
    format.Format = {WAVE_FORMAT_EXTENSIBLE, 2, 48000, 384000, 8, 32,
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)};
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    if (SUCCEEDED(result)) result = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &format.Format, nullptr);
    if (SUCCEEDED(result)) result = client_->SetEventHandle(event);
    if (SUCCEEDED(result)) result = client_->GetBufferSize(&frames_);
    if (SUCCEEDED(result)) result = client_->GetService(IID_PPV_ARGS(&render_));
    if (SUCCEEDED(result)) result = client_->GetService(IID_PPV_ARGS(&clock_));
    if (SUCCEEDED(result)) result = clock_->GetFrequency(&frequency_);
    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(result)) result = client_->GetStreamLatency(&latency);
    if (SUCCEEDED(result) && latency >= 0 && latency < 100000000)
      latency_ = (std::max)(frames_, static_cast<UINT32>((latency * 48000 + 9999999) / 10000000));
    if (SUCCEEDED(result) && (frames_ == 0 || frames_ > 96000 || frequency_ == 0 ||
        frequency_ > (std::numeric_limits<UINT64>::max)() / 48000)) result = E_FAIL;
    return result;
  }
  void close() noexcept override { clock_.Reset(); render_.Reset(); client_.Reset(); }
  UINT32 capacity() const noexcept override { return frames_; }
  UINT32 latency_frames() const noexcept override { return latency_; }
  HRESULT start() noexcept override { return client_->Start(); }
  HRESULT stop() noexcept override { return client_->Stop(); }
  HRESULT reset() noexcept override { return client_->Reset(); }
  HRESULT acquire(UINT32* frames, float** data) noexcept override {
    UINT32 padding = 0;
    HRESULT result = client_->GetCurrentPadding(&padding);
    if (FAILED(result)) return result;
    if (padding > frames_) return E_FAIL;
    *frames = frames_ - padding; *data = nullptr;
    return *frames == 0 ? S_OK : render_->GetBuffer(*frames, reinterpret_cast<BYTE**>(data));
  }
  HRESULT release(UINT32 frames) noexcept override { return render_->ReleaseBuffer(frames, 0); }
  HRESULT position(UINT64* frames, UINT64* nanoseconds) noexcept override {
    UINT64 ticks = 0, qpc = 0;
    const auto result = clock_->GetPosition(&ticks, &qpc);
    if (SUCCEEDED(result)) {
      *frames = ticks / frequency_ * 48000 + ticks % frequency_ * 48000 / frequency_;
      *nanoseconds = qpc <= (std::numeric_limits<UINT64>::max)() / 100 ? qpc * 100 : 0;
    }
    return result;
  }
 private:
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioRenderClient> render_;
  ComPtr<IAudioClock> clock_;
  UINT32 frames_ = 0;
  UINT32 latency_ = 0;
  UINT64 frequency_ = 0;
};
}
std::unique_ptr<IWindowsRenderDevice> make_windows_render_device() {
  return std::make_unique<WasapiDevice>();
}
} // namespace soundfork
