#pragma once

#include <array>
#include <atomic>
#include <vector>

#include "audio-bridge/backend_factory.hpp"
#include "capture-backend/shared_ring.hpp"

namespace soundfork {

class ProxyDriver final : public IASIO {
 public:
  explicit ProxyDriver(
      IBackendFactory& backend_factory,
      const wchar_t* capture_mapping_name = capture::kDefaultMappingName) noexcept;
  ~ProxyDriver();

  ProxyDriver(const ProxyDriver&) = delete;
  ProxyDriver& operator=(const ProxyDriver&) = delete;

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

  static long live_instances() noexcept;

 private:
  enum class State {
    created,
    initialized,
    prepared,
    running,
  };

  static void on_buffer_switch(long index, ASIOBool direct_process) noexcept;
  static void on_sample_rate_changed(ASIOSampleRate sample_rate) noexcept;
  static long on_asio_message(
      long selector,
      long value,
      void* message,
      double* option) noexcept;
  static ASIOTime* on_buffer_switch_time_info(
      ASIOTime* time_info,
      long index,
      ASIOBool direct_process) noexcept;

  void set_error(const char* message) noexcept;
  void set_hresult_error(const char* operation, HRESULT result) noexcept;
  void prepare_capture(long buffer_size) noexcept;
  void begin_callback(long buffer_index) noexcept;
  void publish_capture() noexcept;
  void finish_callback() noexcept;
  void release_backend() noexcept;

  std::atomic<ULONG> reference_count_{1};
  IBackendFactory& backend_factory_;
  IASIO* backend_ = nullptr;
  State state_ = State::created;
  ASIOCallbacks host_callbacks_{};
  ASIOCallbacks downstream_callbacks_{};
  std::vector<ASIOBufferInfo> downstream_buffers_;
  capture::SharedRingPublisher capture_publisher_;
  std::array<wchar_t, 128> capture_mapping_name_{};
  long capture_left_index_ = -1;
  long capture_right_index_ = -1;
  long callback_buffer_index_ = 0;
  bool in_callback_ = false;
  bool capture_published_for_callback_ = false;
  std::array<char, 124> error_message_{};

  static std::atomic<ProxyDriver*> active_driver_;
  static std::atomic<long> live_instances_;
};

}  // namespace soundfork
