#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "audio-bridge/backend_factory.hpp"

namespace soundfork {
struct WindowsOutputInfo {
  UINT32 capacity = 0, period = 0, queue_target = 0;
  HRESULT low_latency_result = S_FALSE;
  UINT32 device_rate = 48000, conversion_delay = 0;
};
// All device methods run on the backend's COM worker, never on the host thread.
class IWindowsRenderDevice {
 public:
  virtual ~IWindowsRenderDevice() = default;
  virtual HRESULT open(const std::wstring& endpoint, HANDLE event) = 0;
  virtual void close() noexcept = 0;
  virtual UINT32 capacity() const noexcept = 0;
  virtual UINT32 latency_frames() const noexcept { return capacity(); }
  virtual WindowsOutputInfo info() const noexcept { return {capacity(), 0, capacity(), S_FALSE}; }
  virtual HRESULT start() noexcept = 0;
  virtual HRESULT stop() noexcept = 0;
  virtual HRESULT reset() noexcept = 0;
  virtual HRESULT acquire(UINT32* frames, float** data) noexcept = 0;
  virtual HRESULT release(UINT32 frames) noexcept = 0;
  virtual HRESULT position(UINT64* frames, UINT64* nanoseconds) noexcept = 0;
};
std::unique_ptr<IWindowsRenderDevice> make_windows_render_device();

class WindowsAudioDriver final : public IASIO {
 public:
  explicit WindowsAudioDriver(std::wstring endpoint,
      std::unique_ptr<IWindowsRenderDevice> device = make_windows_render_device());
  ~WindowsAudioDriver();
  // Read after successful createBuffers, before disposeBuffers (serialized ASIO lifecycle).
  WindowsOutputInfo output_info() const noexcept { return output_info_; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  ASIOBool init(void*) override;
  void getDriverName(char*) override;
  long getDriverVersion() override { return 1; }
  void getErrorMessage(char*) override;
  ASIOError start() override;
  ASIOError stop() override;
  ASIOError getChannels(long*, long*) override;
  ASIOError getLatencies(long*, long*) override;
  ASIOError getBufferSize(long*, long*, long*, long*) override;
  ASIOError canSampleRate(ASIOSampleRate rate) override { return rate == 48000.0 ? ASE_OK : ASE_NoClock; }
  ASIOError getSampleRate(ASIOSampleRate* rate) override;
  ASIOError setSampleRate(ASIOSampleRate rate) override;
  ASIOError getClockSources(ASIOClockSource*, long*) override;
  ASIOError setClockSource(long source) override { return source == 0 ? ASE_OK : ASE_NotPresent; }
  ASIOError getSamplePosition(ASIOSamples*, ASIOTimeStamp*) override;
  ASIOError getChannelInfo(ASIOChannelInfo*) override;
  ASIOError createBuffers(ASIOBufferInfo*, long, long, ASIOCallbacks*) override;
  ASIOError disposeBuffers() override;
  ASIOError controlPanel() override { return ASE_NotPresent; }
  ASIOError future(long, void*) override;
  ASIOError outputReady() override { return ASE_OK; }
 private:
  ASIOError command(bool running);
  void worker() noexcept;
  HRESULT render() noexcept;
  void fill_block() noexcept;
  std::atomic<ULONG> references_{1};
  std::wstring endpoint_;
  std::unique_ptr<IWindowsRenderDevice> device_;
  std::array<std::array<std::vector<float>, 2>, 2> buffers_;
  std::array<bool, 2> enabled_{};
  ASIOCallbacks callbacks_{};
  std::thread thread_;
  HANDLE shutdown_ = nullptr, control_ = nullptr, ready_ = nullptr, audio_ = nullptr;
  std::atomic<DWORD> worker_id_{0};
  std::atomic<bool> requested_running_{false}, running_{false};
  std::atomic<HRESULT> result_{S_OK};
  std::atomic<UINT64> position_{0}, timestamp_{0};
  UINT32 endpoint_frames_ = 0, latency_frames_ = 0;
  WindowsOutputInfo output_info_{};
  long frames_ = 0, index_ = 0, offset_ = 0;
  UINT64 generated_ = 0, callback_timestamp_ = 0;
  bool initialized_ = false, time_info_ = false, callback_active_ = false;
};
} // namespace soundfork
