#include "audio-bridge/windows_audio_driver.hpp"
#include "audio-bridge/ids.hpp"
#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace soundfork {
namespace {
template<class T = ASIOSamples> T split(UINT64 value) noexcept {
  return {static_cast<unsigned long>(value >> 32), static_cast<unsigned long>(value)};
}
}
WindowsAudioDriver::WindowsAudioDriver(std::wstring endpoint, std::unique_ptr<IWindowsRenderDevice> device)
    : endpoint_(std::move(endpoint)), device_(std::move(device)) {}
WindowsAudioDriver::~WindowsAudioDriver() { (void)disposeBuffers(); }
HRESULT WindowsAudioDriver::QueryInterface(REFIID id, void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (!IsEqualIID(id, IID_IUnknown) && !IsEqualIID(id, kSoundForkClsid)) return E_NOINTERFACE;
  *object = static_cast<IASIO*>(this); AddRef(); return S_OK;
}
ULONG WindowsAudioDriver::AddRef() { return references_.fetch_add(1) + 1; }
ULONG WindowsAudioDriver::Release() { const auto left = references_.fetch_sub(1) - 1; if (!left) delete this; return left; }
ASIOBool WindowsAudioDriver::init(void*) {
  if (initialized_ || endpoint_.empty() || !device_) return ASIOFalse;
  initialized_ = true; return ASIOTrue;
}
void WindowsAudioDriver::getDriverName(char* name) { if(name) strcpy_s(name, 32, "SoundFork Windows Output"); }
void WindowsAudioDriver::getErrorMessage(char* text) {
  if(text) sprintf_s(text, 124, "Windows output HRESULT 0x%08lX; check selected playback device", static_cast<unsigned long>(result_.load()));
}
ASIOError WindowsAudioDriver::getChannels(long* input, long* output) {
  if(!input || !output) return ASE_InvalidParameter; *input = 0; *output = 2; return ASE_OK;
}
ASIOError WindowsAudioDriver::getLatencies(long* input, long* output) {
  if(!input || !output) return ASE_InvalidParameter;
  *input = 0; *output = static_cast<long>(latency_frames_) + (frames_ ? frames_ : 256); return ASE_OK;
}
ASIOError WindowsAudioDriver::getBufferSize(long* min, long* max, long* preferred, long* granularity) {
  if(!min || !max || !preferred || !granularity) return ASE_InvalidParameter;
  *min = 64; *max = 2048; *preferred = 256; *granularity = -1; return ASE_OK;
}
ASIOError WindowsAudioDriver::getSampleRate(ASIOSampleRate* rate) { if(!rate) return ASE_InvalidParameter; *rate = 48000; return ASE_OK; }
ASIOError WindowsAudioDriver::setSampleRate(ASIOSampleRate rate) { return running_ ? ASE_InvalidMode : canSampleRate(rate); }
ASIOError WindowsAudioDriver::getClockSources(ASIOClockSource* clocks, long* count) {
  if(!count || !clocks || *count < 1) return ASE_InvalidParameter;
  *count = 1; clocks[0] = {}; clocks[0].index = 0; clocks[0].associatedChannel = -1;
  clocks[0].associatedGroup = -1; clocks[0].isCurrentSource = ASIOTrue;
  strcpy_s(clocks[0].name, "Windows device clock"); return ASE_OK;
}
ASIOError WindowsAudioDriver::getSamplePosition(ASIOSamples* position, ASIOTimeStamp* time) {
  if(!position || !time) return ASE_InvalidParameter;
  const bool in_callback = GetCurrentThreadId() == worker_id_ && callback_active_;
  *position = split(in_callback ? generated_ : position_.load(std::memory_order_acquire));
  *time = split<ASIOTimeStamp>(in_callback ? callback_timestamp_ : timestamp_.load(std::memory_order_acquire)); return ASE_OK;
}
ASIOError WindowsAudioDriver::getChannelInfo(ASIOChannelInfo* info) {
  if(!info || info->isInput || info->channel < 0 || info->channel >= 2) return ASE_InvalidParameter;
  info->isActive = enabled_[info->channel] ? ASIOTrue : ASIOFalse;
  info->type = ASIOSTFloat32LSB; info->channelGroup = 0;
  strcpy_s(info->name, info->channel == 0 ? "Windows left" : "Windows right"); return ASE_OK;
}
ASIOError WindowsAudioDriver::future(long selector, void*) {
  return selector == kAsioCanTimeInfo ? ASE_SUCCESS : ASE_NotPresent;
}
ASIOError WindowsAudioDriver::createBuffers(ASIOBufferInfo* infos, long channels, long frames, ASIOCallbacks* callbacks) {
  if(!initialized_ || thread_.joinable()) return ASE_InvalidMode;
  if(!infos || !callbacks || !callbacks->bufferSwitch || channels < 1 || channels > 2 ||
      frames < 64 || frames > 2048 || (frames & (frames - 1)) != 0) return ASE_InvalidParameter;
  enabled_ = {};
  for(long i = 0; i < channels; ++i) {
    if(infos[i].isInput || infos[i].channelNum < 0 || infos[i].channelNum > 1 || enabled_[infos[i].channelNum]) return ASE_InvalidParameter;
    enabled_[infos[i].channelNum] = true;
  }
  frames_ = frames; callbacks_ = *callbacks;
  try { for(auto& channel : buffers_) for(auto& buffer : channel) buffer.assign(static_cast<std::size_t>(frames), 0.0F); }
  catch (...) { return ASE_NoMemory; }
  // All sample pages are written before the worker can invoke a callback.
  for(long i = 0; i < channels; ++i) for(int j = 0; j < 2; ++j)
    infos[i].buffers[j] = buffers_[infos[i].channelNum][j].data();
  shutdown_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  control_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  audio_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if(!shutdown_ || !control_ || !ready_ || !audio_) { disposeBuffers(); return ASE_NoMemory; }
  time_info_ = callbacks_.asioMessage && callbacks_.bufferSwitchTimeInfo &&
      callbacks_.asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) != 0;
  try { thread_ = std::thread(&WindowsAudioDriver::worker, this); }
  catch (...) { disposeBuffers(); return ASE_NoMemory; }
  WaitForSingleObject(ready_, INFINITE); // lifecycle only
  if(FAILED(result_)) { disposeBuffers(); return ASE_HWMalfunction; }
  return ASE_OK;
}
ASIOError WindowsAudioDriver::command(bool run) {
  if(!thread_.joinable() || GetCurrentThreadId() == worker_id_) return ASE_InvalidMode;
  ResetEvent(ready_); requested_running_ = run; SetEvent(control_);
  WaitForSingleObject(ready_, INFINITE); // never called from a callback
  return FAILED(result_) ? ASE_HWMalfunction : ASE_OK;
}
ASIOError WindowsAudioDriver::start() { return running_ ? ASE_InvalidMode : command(true); }
ASIOError WindowsAudioDriver::stop() { return command(false); }
ASIOError WindowsAudioDriver::disposeBuffers() {
  if(thread_.joinable() && GetCurrentThreadId() == worker_id_) return ASE_InvalidMode;
  if(shutdown_) SetEvent(shutdown_);
  if(thread_.joinable()) thread_.join();
  for(auto* handle : {&shutdown_, &control_, &ready_, &audio_}) {
    if(*handle) CloseHandle(*handle); *handle = nullptr;
  }
  running_ = false; worker_id_ = 0; enabled_ = {}; callbacks_ = {}; endpoint_frames_ = 0;
  for(auto& channel : buffers_) for(auto& buffer : channel) buffer.clear();
  return ASE_OK;
}
void WindowsAudioDriver::fill_block() noexcept {
  for(int channel = 0; channel < 2; ++channel)
    std::fill(buffers_[channel][index_].begin(), buffers_[channel][index_].end(), 0.0F);
  callback_active_ = true;
  const auto played = position_.load();
  const auto ahead = generated_ > played ? generated_ - played : 0;
  callback_timestamp_ = timestamp_.load() + ahead / 48000 * 1000000000ULL + ahead % 48000 * 1000000000ULL / 48000;
  if(time_info_) {
    ASIOTime time{};
    time.timeInfo.speed = 1.0; time.timeInfo.sampleRate = 48000;
    time.timeInfo.samplePosition = split(generated_); time.timeInfo.systemTime = split<ASIOTimeStamp>(callback_timestamp_);
    time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
    callbacks_.bufferSwitchTimeInfo(&time, index_, ASIOTrue);
  } else callbacks_.bufferSwitch(index_, ASIOTrue);
  callback_active_ = false;
  generated_ += static_cast<UINT64>(frames_);
}
HRESULT WindowsAudioDriver::render() noexcept {
  UINT64 played = 0, time = 0;
  auto result = device_->position(&played, &time);
  if(FAILED(result)) return result;
  if(time == 0) {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
    const auto ticks = static_cast<UINT64>(counter.QuadPart);
    const auto rate = static_cast<UINT64>(frequency.QuadPart);
    time = ticks / rate * 1000000000ULL + ticks % rate * 1000000000ULL / rate;
  }
  position_.store(played, std::memory_order_release); timestamp_.store(time, std::memory_order_release);
  UINT32 available = 0; float* destination = nullptr;
  result = device_->acquire(&available, &destination);
  if(FAILED(result) || available == 0) return result;
  if(!destination || available > endpoint_frames_) return E_UNEXPECTED;
  for(UINT32 frame = 0; frame < available; ++frame) {
    if(offset_ == frames_) { offset_ = 0; index_ ^= 1; }
    if(offset_ == 0) fill_block();
    for(int channel = 0; channel < 2; ++channel) {
      const auto sample = buffers_[channel][index_][static_cast<std::size_t>(offset_)];
      destination[frame * 2 + channel] = std::isfinite(sample) ? (std::clamp)(sample, -1.0F, 1.0F) : 0.0F;
    }
    ++offset_;
  }
  return device_->release(available);
}
void WindowsAudioDriver::worker() noexcept {
  worker_id_ = GetCurrentThreadId();
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HRESULT result = FAILED(com) ? com : device_->open(endpoint_, audio_);
  endpoint_frames_ = SUCCEEDED(result) ? device_->capacity() : 0;
  latency_frames_ = SUCCEEDED(result) ? device_->latency_frames() : 0;
  result_ = result; SetEvent(ready_);
  if(SUCCEEDED(result)) {
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    const HANDLE handles[] = {shutdown_, control_, audio_};
    for(;;) {
      const auto wake = WaitForMultipleObjects(3, handles, FALSE, 1000);
      if(wake == WAIT_OBJECT_0 || wake == WAIT_FAILED) break;
      if(wake == WAIT_OBJECT_0 + 1) {
        if(requested_running_) {
          result = device_->reset(); index_ = 0; offset_ = 0; generated_ = 0; position_ = 0; timestamp_ = 0;
          if(SUCCEEDED(result)) result = render();
          if(SUCCEEDED(result)) result = device_->start();
          running_ = SUCCEEDED(result);
        } else { result = device_->stop(); running_ = false; }
        result_ = result; SetEvent(ready_);
      } else if(wake == WAIT_OBJECT_0 + 2 && running_) {
        result = render();
        if(FAILED(result)) {
          result_ = result; running_ = false; (void)device_->stop();
          // The host owns ASIO reset/recreate policy. Do not silently change devices.
          if(callbacks_.asioMessage) callbacks_.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
      } else if(wake == WAIT_TIMEOUT && running_) {
        UINT64 played = 0, stamp = 0;
        result = device_->position(&played, &stamp);
        if(FAILED(result)) {
          result_ = result; running_ = false; (void)device_->stop();
          if(callbacks_.asioMessage) callbacks_.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
      }
    }
    (void)device_->stop();
    if(mmcss) AvRevertMmThreadCharacteristics(mmcss);
  }
  device_->close();
  if(SUCCEEDED(com)) CoUninitialize();
}
} // namespace soundfork
