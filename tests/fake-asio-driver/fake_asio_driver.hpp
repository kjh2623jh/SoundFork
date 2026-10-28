#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>

#include "audio-bridge/backend_factory.hpp"

namespace soundfork::test {

// {674EA0F7-C93A-46C2-894C-6B8D1B5F564C}
inline constexpr CLSID kFakeDriverClsid{
    0x674ea0f7,
    0xc93a,
    0x46c2,
    {0x89, 0x4c, 0x6b, 0x8d, 0x1b, 0x5f, 0x56, 0x4c}};

struct FakeAsioConfig {
  long frames_per_block = 64;
  ASIOSampleRate sample_rate = 48000.0;
  std::array<ASIOSampleType, 2> channel_types{ASIOSTInt32LSB, ASIOSTInt32LSB};
};

class FakeAsioDriver final : public IASIO {
 public:
  explicit FakeAsioDriver(FakeAsioConfig config = {}) noexcept;
  ~FakeAsioDriver() = default;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  ASIOBool init(void* system_handle) override;
  void getDriverName(char* name) override;
  long getDriverVersion() override;
  void getErrorMessage(char* message) override;
  ASIOError start() override;
  ASIOError stop() override;
  ASIOError getChannels(long* input_channels, long* output_channels) override;
  ASIOError getLatencies(long* input_latency, long* output_latency) override;
  ASIOError getBufferSize(
      long* min_size,
      long* max_size,
      long* preferred_size,
      long* granularity) override;
  ASIOError canSampleRate(ASIOSampleRate sample_rate) override;
  ASIOError getSampleRate(ASIOSampleRate* sample_rate) override;
  ASIOError setSampleRate(ASIOSampleRate sample_rate) override;
  ASIOError getClockSources(ASIOClockSource* clocks, long* count) override;
  ASIOError setClockSource(long reference) override;
  ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* timestamp) override;
  ASIOError getChannelInfo(ASIOChannelInfo* info) override;
  ASIOError createBuffers(
      ASIOBufferInfo* buffer_infos,
      long channel_count,
      long buffer_size,
      ASIOCallbacks* callbacks) override;
  ASIOError disposeBuffers() override;
  ASIOError controlPanel() override;
  ASIOError future(long selector, void* option) override;
  ASIOError outputReady() override;

  void fire_buffer_switch(long index, ASIOBool direct_process = ASIOTrue) noexcept;
  ASIOTime* fire_time_info(long index, ASIOTime* time_info) noexcept;
  const std::int32_t* output(long channel, long index) const noexcept;
  const void* output_data(long channel, long index) const noexcept;
  std::size_t output_bytes(long channel) const noexcept;
  long output_ready_calls() const noexcept;
  void set_channel_type(long channel, ASIOSampleType type) noexcept { if (channel >= 0 && channel < 2) types_[channel] = type; }
  void notify_sample_rate(ASIOSampleRate rate) noexcept {
    sample_rate_ = rate;
    if (callbacks_ && callbacks_->sampleRateDidChange) callbacks_->sampleRateDidChange(rate);
  }
  void request_reset() noexcept {
    if (callbacks_ && callbacks_->asioMessage) callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
  }

 private:
  std::atomic<ULONG> reference_count_{1};
  ASIOCallbacks* callbacks_ = nullptr;
  FakeAsioConfig config_{};
  std::array<std::array<std::unique_ptr<std::byte[]>, 2>, 2> outputs_{};
  std::array<std::size_t, 2> output_bytes_{};
  ASIOSampleRate sample_rate_ = 48000.0;
  std::array<ASIOSampleType, 2> types_{ASIOSTInt32LSB, ASIOSTInt32LSB};
  std::uint64_t sample_position_ = 0;
  bool initialized_ = false;
  bool prepared_ = false;
  bool running_ = false;
  std::atomic<long> output_ready_calls_{0};
};

class FakeBackendFactory final : public IBackendFactory {
 public:
  explicit FakeBackendFactory(FakeAsioConfig config = {}) noexcept : config_(config) {}
  HRESULT create(IASIO** driver) noexcept override;
  FakeAsioDriver* last_created() const noexcept;

 private:
  FakeAsioConfig config_{};
  FakeAsioDriver* last_created_ = nullptr;
};

}  // namespace soundfork::test
