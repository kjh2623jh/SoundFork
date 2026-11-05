#include "audio-bridge/windows_audio_driver.hpp"
#include "audio-bridge/windows_render_policy.hpp"
#include "audio-bridge/windows_resampler.hpp"
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
    WAVEFORMATEXTENSIBLE format{};
    format.Format = {WAVE_FORMAT_EXTENSIBLE, 2, 48000, 384000, 8, 32,
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)};
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    WAVEFORMATEX* mix = nullptr;
    ComPtr<IAudioClient> inspection;
    if(SUCCEEDED(result)) result = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
        nullptr, reinterpret_cast<void**>(inspection.GetAddressOf()));
    if(SUCCEEDED(result)) result = inspection->GetMixFormat(&mix);
    if(SUCCEEDED(result)) {
      const bool floating = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
          (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22 &&
           IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
      if(floating && mix->nChannels == 2 && mix->wBitsPerSample == 32 && mix->nBlockAlign == 8 &&
          mix->nSamplesPerSec >= 32000 && mix->nSamplesPerSec <= 384000) {
        native_rate_ = mix->nSamplesPerSec;
        if(mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
          format = *reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
          format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        }
        else { format.Format.nSamplesPerSec = native_rate_; format.Format.nAvgBytesPerSec = native_rate_ * 8; }
      }
    }
    if(mix) CoTaskMemFree(mix);
    inspection.Reset();
    converting_ = native_rate_ != 48000;
    if(SUCCEEDED(result) && converting_ && !resampler_.prepare(native_rate_)) result = E_INVALIDARG;
    if (SUCCEEDED(result)) {
      ComPtr<IAudioClient3> client3;
      result = initialize_shared_render(
          [&](UINT32* fundamental, UINT32* minimum, UINT32* maximum) {
            auto status = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                nullptr, reinterpret_cast<void**>(client_.ReleaseAndGetAddressOf()));
            if (SUCCEEDED(status)) status = client_.As(&client3);
            UINT32 normal = 0;
            if (SUCCEEDED(status)) status = client3->GetSharedModeEnginePeriod(
                &format.Format, &normal, fundamental, minimum, maximum);
            return status;
          },
          [&](UINT32 period) {
            // InitializeSharedAudioStream supports EVENTCALLBACK, not AUTOCONVERTPCM.
            return client3->InitializeSharedAudioStream(
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, &format.Format, nullptr);
          },
          [&] {
            // Failed low-latency negotiation must not prevent ordinary shared playback.
            client3.Reset(); client_.Reset();
            auto status = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                nullptr, reinterpret_cast<void**>(client_.GetAddressOf()));
            if (SUCCEEDED(status)) status = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                0, 0, &format.Format, nullptr);
            return status;
          }, &period_, &low_latency_result_);
    }
    if (SUCCEEDED(result)) result = client_->SetEventHandle(event);
    if (SUCCEEDED(result)) result = client_->GetBufferSize(&frames_);
    if(SUCCEEDED(result) && (frames_ == 0 || frames_ > 96000)) result = E_FAIL;
    target_ = shared_queue_target(frames_, period_);
    if(SUCCEEDED(result)) {
      source_capacity_ = static_cast<UINT32>((static_cast<UINT64>(frames_) * 48000 + native_rate_ - 1) / native_rate_) + 2;
      if(converting_) staging_.assign(static_cast<std::size_t>(source_capacity_) * 2, 0.0F);
    }
    if (SUCCEEDED(result)) result = client_->GetService(IID_PPV_ARGS(&render_));
    if (SUCCEEDED(result)) result = client_->GetService(IID_PPV_ARGS(&clock_));
    if (SUCCEEDED(result)) result = clock_->GetFrequency(&frequency_);
    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(result)) result = client_->GetStreamLatency(&latency);
    if (SUCCEEDED(result) && latency >= 0 && latency < 100000000)
      latency_ = (std::max)(static_cast<UINT32>((static_cast<UINT64>(target_) * 48000 + native_rate_ - 1) / native_rate_),
          static_cast<UINT32>((latency * 48000 + 9999999) / 10000000)) + (converting_ ? WindowsResampler::taps / 2 : 0);
    if (SUCCEEDED(result) && (frames_ == 0 || frames_ > 96000 || frequency_ == 0 ||
        frequency_ > (std::numeric_limits<UINT64>::max)() / 48000)) result = E_FAIL;
    return result;
  }
  void close() noexcept override { clock_.Reset(); render_.Reset(); client_.Reset(); }
  UINT32 capacity() const noexcept override { return source_capacity_; }
  UINT32 latency_frames() const noexcept override { return latency_; }
  WindowsOutputInfo info() const noexcept override {
    return {frames_, period_, target_, low_latency_result_, native_rate_, converting_ ? WindowsResampler::taps / 2 : 0};
  }
  HRESULT start() noexcept override { return client_->Start(); }
  HRESULT stop() noexcept override { return client_->Stop(); }
  HRESULT reset() noexcept override { resampler_.reset(); return client_->Reset(); }
  HRESULT acquire(UINT32* frames, float** data) noexcept override {
    UINT32 padding = 0;
    HRESULT result = client_->GetCurrentPadding(&padding);
    if (FAILED(result)) return result;
    if (padding > frames_) return E_FAIL;
    pending_output_ = shared_write_frames(frames_, target_, padding);
    *frames = converting_ ? resampler_.input_frames(pending_output_) : pending_output_; *data = nullptr;
    if(converting_) {
      if(*frames > source_capacity_) return E_UNEXPECTED;
      if(!pending_output_) return S_OK;
      if(!*frames) return commit_conversion();
      *data = staging_.data(); return S_OK;
    }
    return *frames == 0 ? S_OK : render_->GetBuffer(*frames, reinterpret_cast<BYTE**>(data));
  }
  HRESULT release(UINT32 frames) noexcept override {
    if(converting_) {
      if(frames != resampler_.input_frames(pending_output_)) return E_INVALIDARG;
      return commit_conversion();
    }
    return render_->ReleaseBuffer(frames, 0);
  }
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
  HRESULT commit_conversion() noexcept {
    BYTE* data = nullptr;
    auto result = render_->GetBuffer(pending_output_, &data);
    if(FAILED(result)) return result;
    resampler_.process(staging_.data(), pending_output_, reinterpret_cast<float*>(data));
    result = render_->ReleaseBuffer(pending_output_, 0); pending_output_ = 0; return result;
  }
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioRenderClient> render_;
  ComPtr<IAudioClock> clock_;
  UINT32 frames_ = 0;
  UINT32 latency_ = 0;
  UINT32 period_ = 0, target_ = 0;
  HRESULT low_latency_result_ = S_FALSE;
  UINT64 frequency_ = 0;
  UINT32 native_rate_ = 48000, source_capacity_ = 0, pending_output_ = 0;
  bool converting_ = false;
  WindowsResampler resampler_;
  std::vector<float> staging_;
};
}
std::unique_ptr<IWindowsRenderDevice> make_windows_render_device() {
  return std::make_unique<WasapiDevice>();
}
} // namespace soundfork
