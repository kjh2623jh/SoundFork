#include "fake_asio_driver.hpp"

#include <cstring>
#include <new>

namespace soundfork::test {

HRESULT STDMETHODCALLTYPE FakeAsioDriver::QueryInterface(REFIID riid, void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kFakeDriverClsid)) {
    *object = static_cast<IASIO*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FakeAsioDriver::AddRef() {
  return reference_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE FakeAsioDriver::Release() {
  const ULONG remaining =
      reference_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

ASIOBool FakeAsioDriver::init(void*) {
  initialized_ = true;
  return ASIOTrue;
}

void FakeAsioDriver::getDriverName(char* name) {
  if (name != nullptr) {
    strcpy_s(name, 32, "SoundFork Test Driver");
  }
}

long FakeAsioDriver::getDriverVersion() {
  return 1;
}

void FakeAsioDriver::getErrorMessage(char* message) {
  if (message != nullptr) {
    strcpy_s(message, 124, "No error");
  }
}

ASIOError FakeAsioDriver::start() {
  if (!prepared_) {
    return ASE_InvalidMode;
  }
  running_ = true;
  return ASE_OK;
}

ASIOError FakeAsioDriver::stop() {
  if (!running_) {
    return ASE_InvalidMode;
  }
  running_ = false;
  return ASE_OK;
}

ASIOError FakeAsioDriver::getChannels(long* input_channels, long* output_channels) {
  if (input_channels == nullptr || output_channels == nullptr) {
    return ASE_InvalidParameter;
  }
  *input_channels = 0;
  *output_channels = 2;
  return ASE_OK;
}

ASIOError FakeAsioDriver::getLatencies(long* input_latency, long* output_latency) {
  if (input_latency == nullptr || output_latency == nullptr) {
    return ASE_InvalidParameter;
  }
  *input_latency = 0;
  *output_latency = 128;
  return ASE_OK;
}

ASIOError FakeAsioDriver::getBufferSize(
    long* min_size, long* max_size, long* preferred_size, long* granularity) {
  if (min_size == nullptr || max_size == nullptr || preferred_size == nullptr ||
      granularity == nullptr) {
    return ASE_InvalidParameter;
  }
  *min_size = 64;
  *max_size = 64;
  *preferred_size = 64;
  *granularity = 0;
  return ASE_OK;
}

ASIOError FakeAsioDriver::canSampleRate(ASIOSampleRate sample_rate) {
  return sample_rate == 48000.0 ? ASE_OK : ASE_NoClock;
}

ASIOError FakeAsioDriver::getSampleRate(ASIOSampleRate* sample_rate) {
  if (sample_rate == nullptr) {
    return ASE_InvalidParameter;
  }
  *sample_rate = sample_rate_;
  return ASE_OK;
}

ASIOError FakeAsioDriver::setSampleRate(ASIOSampleRate sample_rate) {
  if (canSampleRate(sample_rate) != ASE_OK) {
    return ASE_NoClock;
  }
  sample_rate_ = sample_rate;
  return ASE_OK;
}

ASIOError FakeAsioDriver::getClockSources(ASIOClockSource* clocks, long* count) {
  if (count == nullptr || *count < 1 || clocks == nullptr) {
    return ASE_InvalidParameter;
  }
  clocks[0] = {};
  clocks[0].index = 0;
  clocks[0].isCurrentSource = ASIOTrue;
  strcpy_s(clocks[0].name, "Internal");
  *count = 1;
  return ASE_OK;
}

ASIOError FakeAsioDriver::setClockSource(long reference) {
  return reference == 0 ? ASE_OK : ASE_NotPresent;
}

ASIOError FakeAsioDriver::getSamplePosition(
    ASIOSamples* position, ASIOTimeStamp* timestamp) {
  if (position == nullptr || timestamp == nullptr) {
    return ASE_InvalidParameter;
  }
  position->hi = static_cast<unsigned long>(sample_position_ >> 32U);
  position->lo = static_cast<unsigned long>(sample_position_ & 0xffffffffU);
  timestamp->hi = 0;
  timestamp->lo = 0;
  return ASE_OK;
}

ASIOError FakeAsioDriver::getChannelInfo(ASIOChannelInfo* info) {
  if (info == nullptr || info->isInput == ASIOTrue || info->channel < 0 ||
      info->channel >= 2) {
    return ASE_InvalidParameter;
  }
  info->isActive = prepared_ ? ASIOTrue : ASIOFalse;
  info->channelGroup = 0;
  info->type = types_[info->channel];
  strcpy_s(info->name, info->channel == 0 ? "Left" : "Right");
  return ASE_OK;
}

ASIOError FakeAsioDriver::createBuffers(
    ASIOBufferInfo* buffer_infos,
    long channel_count,
    long buffer_size,
    ASIOCallbacks* callbacks) {
  if (!initialized_ || prepared_) {
    return ASE_InvalidMode;
  }
  if (buffer_infos == nullptr || callbacks == nullptr || channel_count <= 0 ||
      channel_count > 2 || buffer_size != 64) {
    return ASE_InvalidParameter;
  }

  for (long index = 0; index < channel_count; ++index) {
    const long channel = buffer_infos[index].channelNum;
    if (buffer_infos[index].isInput == ASIOTrue || channel < 0 || channel >= 2) {
      return ASE_InvalidParameter;
    }
    buffer_infos[index].buffers[0] = outputs_[channel][0].data();
    buffer_infos[index].buffers[1] = outputs_[channel][1].data();
  }
  callbacks_ = callbacks;
  prepared_ = true;
  if (callbacks_->asioMessage != nullptr) {
    callbacks_->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr);
  }
  return ASE_OK;
}

ASIOError FakeAsioDriver::disposeBuffers() {
  if (!prepared_ || running_) {
    return ASE_InvalidMode;
  }
  callbacks_ = nullptr;
  prepared_ = false;
  return ASE_OK;
}

ASIOError FakeAsioDriver::controlPanel() {
  return ASE_OK;
}

ASIOError FakeAsioDriver::future(long selector, void*) {
  return selector == kAsioCanTimeInfo ? ASE_SUCCESS : ASE_NotPresent;
}

ASIOError FakeAsioDriver::outputReady() {
  output_ready_calls_.fetch_add(1, std::memory_order_relaxed);
  return ASE_OK;
}

void FakeAsioDriver::fire_buffer_switch(long index, ASIOBool direct_process) noexcept {
  if (callbacks_ != nullptr && callbacks_->bufferSwitch != nullptr) {
    callbacks_->bufferSwitch(index, direct_process);
    sample_position_ += 64;
  }
}

ASIOTime* FakeAsioDriver::fire_time_info(long index, ASIOTime* time_info) noexcept {
  if (callbacks_ == nullptr || callbacks_->bufferSwitchTimeInfo == nullptr) {
    return time_info;
  }
  ASIOTime* result = callbacks_->bufferSwitchTimeInfo(
      time_info, index, ASIOTrue);
  sample_position_ += 64;
  return result;
}

const std::int32_t* FakeAsioDriver::output(long channel, long index) const noexcept {
  if (channel < 0 || channel >= 2 || index < 0 || index >= 2) {
    return nullptr;
  }
  return outputs_[channel][index].data();
}

long FakeAsioDriver::output_ready_calls() const noexcept {
  return output_ready_calls_.load(std::memory_order_relaxed);
}

HRESULT FakeBackendFactory::create(IASIO** driver) noexcept {
  if (driver == nullptr) {
    return E_POINTER;
  }
  *driver = new (std::nothrow) FakeAsioDriver();
  last_created_ = static_cast<FakeAsioDriver*>(*driver);
  return *driver == nullptr ? E_OUTOFMEMORY : S_OK;
}

FakeAsioDriver* FakeBackendFactory::last_created() const noexcept {
  return last_created_;
}

}  // namespace soundfork::test
