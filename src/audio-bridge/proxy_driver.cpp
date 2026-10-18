#include "audio-bridge/proxy_driver.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "audio-bridge/ids.hpp"

namespace soundfork {

std::atomic<ProxyDriver*> ProxyDriver::active_driver_{nullptr};
std::atomic<long> ProxyDriver::live_instances_{0};

namespace {

constexpr long kDriverVersion = 0x00010000L;

}  // namespace

ProxyDriver::ProxyDriver(
    IBackendFactory& backend_factory,
    const wchar_t* capture_mapping_name) noexcept
    : backend_factory_(backend_factory) {
  wcsncpy_s(
      capture_mapping_name_.data(),
      capture_mapping_name_.size(),
      capture_mapping_name == nullptr ? capture::kDefaultMappingName
                                      : capture_mapping_name,
      _TRUNCATE);
  live_instances_.fetch_add(1, std::memory_order_relaxed);
}

ProxyDriver::~ProxyDriver() {
  if (state_ == State::running) {
    backend_->stop();
    state_ = State::prepared;
  }
  if (state_ == State::prepared) {
    capture_publisher_.close();
    backend_->disposeBuffers();
    ProxyDriver* expected = this;
    active_driver_.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel);
  }
  release_backend();
  live_instances_.fetch_sub(1, std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE ProxyDriver::QueryInterface(REFIID riid, void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;

  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kSoundForkClsid)) {
    *object = static_cast<IASIO*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ProxyDriver::AddRef() {
  return reference_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE ProxyDriver::Release() {
  const ULONG remaining =
      reference_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

ASIOBool ProxyDriver::init(void* system_handle) {
  if (state_ != State::created) {
    set_error("SoundFork is already initialized");
    return ASIOFalse;
  }

  const HRESULT result = backend_factory_.create(&backend_);
  if (FAILED(result) || backend_ == nullptr) {
    set_hresult_error("Cannot create configured backend", result);
    backend_ = nullptr;
    return ASIOFalse;
  }

  if (backend_->init(system_handle) != ASIOTrue) {
    std::array<char, 124> backend_error{};
    backend_->getErrorMessage(backend_error.data());
    set_error(backend_error[0] == '\0' ? "Backend initialization failed"
                                        : backend_error.data());
    release_backend();
    return ASIOFalse;
  }

  state_ = State::initialized;
  error_message_[0] = '\0';
  return ASIOTrue;
}

void ProxyDriver::getDriverName(char* name) {
  if (name != nullptr) {
    strcpy_s(name, 32, kDriverNameNarrow);
  }
}

long ProxyDriver::getDriverVersion() {
  return kDriverVersion;
}

void ProxyDriver::getErrorMessage(char* message) {
  if (message == nullptr) {
    return;
  }
  if (error_message_[0] != '\0') {
    strcpy_s(message, 124, error_message_.data());
    return;
  }
  if (backend_ != nullptr) {
    backend_->getErrorMessage(message);
    return;
  }
  strcpy_s(message, 124, "SoundFork is not initialized");
}

ASIOError ProxyDriver::start() {
  if (state_ != State::prepared || backend_ == nullptr) {
    return ASE_InvalidMode;
  }
  const ASIOError result = backend_->start();
  if (result == ASE_OK || result == ASE_SUCCESS) {
    state_ = State::running;
  }
  return result;
}

ASIOError ProxyDriver::stop() {
  if (state_ != State::running || backend_ == nullptr) {
    return ASE_InvalidMode;
  }
  const ASIOError result = backend_->stop();
  if (result == ASE_OK || result == ASE_SUCCESS) {
    state_ = State::prepared;
  }
  return result;
}

ASIOError ProxyDriver::getChannels(long* input_channels, long* output_channels) {
  return backend_ == nullptr ? ASE_NotPresent
                             : backend_->getChannels(input_channels, output_channels);
}

ASIOError ProxyDriver::getLatencies(long* input_latency, long* output_latency) {
  return backend_ == nullptr ? ASE_NotPresent
                             : backend_->getLatencies(input_latency, output_latency);
}

ASIOError ProxyDriver::getBufferSize(
    long* min_size,
    long* max_size,
    long* preferred_size,
    long* granularity) {
  return backend_ == nullptr
             ? ASE_NotPresent
             : backend_->getBufferSize(
                   min_size, max_size, preferred_size, granularity);
}

ASIOError ProxyDriver::canSampleRate(ASIOSampleRate sample_rate) {
  return backend_ == nullptr ? ASE_NotPresent : backend_->canSampleRate(sample_rate);
}

ASIOError ProxyDriver::getSampleRate(ASIOSampleRate* sample_rate) {
  return backend_ == nullptr ? ASE_NotPresent : backend_->getSampleRate(sample_rate);
}

ASIOError ProxyDriver::setSampleRate(ASIOSampleRate sample_rate) {
  return backend_ == nullptr ? ASE_NotPresent : backend_->setSampleRate(sample_rate);
}

ASIOError ProxyDriver::getClockSources(ASIOClockSource* clocks, long* count) {
  return backend_ == nullptr ? ASE_NotPresent : backend_->getClockSources(clocks, count);
}

ASIOError ProxyDriver::setClockSource(long reference) {
  return backend_ == nullptr ? ASE_NotPresent : backend_->setClockSource(reference);
}

ASIOError ProxyDriver::getSamplePosition(
    ASIOSamples* position, ASIOTimeStamp* timestamp) {
  return backend_ == nullptr
             ? ASE_NotPresent
             : backend_->getSamplePosition(position, timestamp);
}

ASIOError ProxyDriver::getChannelInfo(ASIOChannelInfo* info) {
  return backend_ == nullptr ? ASE_NotPresent : backend_->getChannelInfo(info);
}

ASIOError ProxyDriver::createBuffers(
    ASIOBufferInfo* buffer_infos,
    long channel_count,
    long buffer_size,
    ASIOCallbacks* callbacks) {
  if (state_ != State::initialized || backend_ == nullptr) {
    return ASE_InvalidMode;
  }
  if (buffer_infos == nullptr || callbacks == nullptr || channel_count <= 0 ||
      buffer_size <= 0) {
    return ASE_InvalidParameter;
  }

  try {
    downstream_buffers_.assign(buffer_infos, buffer_infos + channel_count);
  } catch (const std::bad_alloc&) {
    return ASE_NoMemory;
  }

  host_callbacks_ = *callbacks;
  ProxyDriver* expected = nullptr;
  if (!active_driver_.compare_exchange_strong(
          expected, this, std::memory_order_acq_rel)) {
    downstream_buffers_.clear();
    host_callbacks_ = {};
    set_error("Only one active SoundFork stream is supported per process");
    return ASE_InvalidMode;
  }

  downstream_callbacks_ = {};
  downstream_callbacks_.bufferSwitch = &ProxyDriver::on_buffer_switch;
  downstream_callbacks_.sampleRateDidChange = &ProxyDriver::on_sample_rate_changed;
  downstream_callbacks_.asioMessage = &ProxyDriver::on_asio_message;
  downstream_callbacks_.bufferSwitchTimeInfo =
      &ProxyDriver::on_buffer_switch_time_info;
  const ASIOError result = backend_->createBuffers(
      downstream_buffers_.data(), channel_count, buffer_size, &downstream_callbacks_);
  if (result != ASE_OK && result != ASE_SUCCESS) {
    expected = this;
    active_driver_.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel);
    downstream_buffers_.clear();
    host_callbacks_ = {};
    downstream_callbacks_ = {};
    return result;
  }

  for (long index = 0; index < channel_count; ++index) {
    buffer_infos[index].buffers[0] = downstream_buffers_[index].buffers[0];
    buffer_infos[index].buffers[1] = downstream_buffers_[index].buffers[1];
  }
  prepare_capture(buffer_size);
  state_ = State::prepared;
  return result;
}

ASIOError ProxyDriver::disposeBuffers() {
  if (state_ != State::prepared || backend_ == nullptr) {
    return ASE_InvalidMode;
  }

  capture_publisher_.close();
  capture_left_index_ = -1;
  capture_right_index_ = -1;
  const ASIOError result = backend_->disposeBuffers();
  ProxyDriver* expected = this;
  active_driver_.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel);
  downstream_buffers_.clear();
  host_callbacks_ = {};
  downstream_callbacks_ = {};
  state_ = State::initialized;
  return result;
}

ASIOError ProxyDriver::controlPanel() {
  return backend_ == nullptr ? ASE_NotPresent : backend_->controlPanel();
}

ASIOError ProxyDriver::future(long selector, void* option) {
  return backend_ == nullptr ? ASE_NotPresent : backend_->future(selector, option);
}

ASIOError ProxyDriver::outputReady() {
  if (backend_ == nullptr) {
    return ASE_NotPresent;
  }
  if (in_callback_ && !capture_published_for_callback_) {
    publish_capture();
  }
  return backend_->outputReady();
}

long ProxyDriver::live_instances() noexcept {
  return live_instances_.load(std::memory_order_relaxed);
}

void ProxyDriver::on_buffer_switch(long index, ASIOBool direct_process) noexcept {
  ProxyDriver* driver = active_driver_.load(std::memory_order_acquire);
  if (driver != nullptr && driver->host_callbacks_.bufferSwitch != nullptr) {
    driver->begin_callback(index);
    driver->host_callbacks_.bufferSwitch(index, direct_process);
    driver->finish_callback();
  }
}

void ProxyDriver::on_sample_rate_changed(ASIOSampleRate sample_rate) noexcept {
  ProxyDriver* driver = active_driver_.load(std::memory_order_acquire);
  if (driver != nullptr && driver->host_callbacks_.sampleRateDidChange != nullptr) {
    driver->host_callbacks_.sampleRateDidChange(sample_rate);
  }
}

long ProxyDriver::on_asio_message(
    long selector, long value, void* message, double* option) noexcept {
  ProxyDriver* driver = active_driver_.load(std::memory_order_acquire);
  if (driver == nullptr || driver->host_callbacks_.asioMessage == nullptr) {
    return 0;
  }
  return driver->host_callbacks_.asioMessage(selector, value, message, option);
}

ASIOTime* ProxyDriver::on_buffer_switch_time_info(
    ASIOTime* time_info, long index, ASIOBool direct_process) noexcept {
  ProxyDriver* driver = active_driver_.load(std::memory_order_acquire);
  if (driver == nullptr) {
    return time_info;
  }
  if (driver->host_callbacks_.bufferSwitchTimeInfo != nullptr) {
    driver->begin_callback(index);
    ASIOTime* result = driver->host_callbacks_.bufferSwitchTimeInfo(
        time_info, index, direct_process);
    driver->finish_callback();
    return result;
  }
  if (driver->host_callbacks_.bufferSwitch != nullptr) {
    driver->begin_callback(index);
    driver->host_callbacks_.bufferSwitch(index, direct_process);
    driver->finish_callback();
  }
  return time_info;
}

void ProxyDriver::set_error(const char* message) noexcept {
  if (message == nullptr) {
    error_message_[0] = '\0';
    return;
  }
  strncpy_s(error_message_.data(), error_message_.size(), message, _TRUNCATE);
}

void ProxyDriver::set_hresult_error(const char* operation, HRESULT result) noexcept {
  _snprintf_s(
      error_message_.data(),
      error_message_.size(),
      _TRUNCATE,
      "%s (HRESULT 0x%08lX)",
      operation,
      static_cast<unsigned long>(result));
}

void ProxyDriver::prepare_capture(long buffer_size) noexcept {
  capture_publisher_.close();
  capture_left_index_ = -1;
  capture_right_index_ = -1;
  if (buffer_size <= 0 ||
      buffer_size > static_cast<long>(capture::kMaxFramesPerBlock)) {
    return;
  }

  for (std::size_t index = 0; index < downstream_buffers_.size(); ++index) {
    const ASIOBufferInfo& buffer = downstream_buffers_[index];
    if (buffer.isInput == ASIOTrue) {
      continue;
    }
    if (buffer.channelNum == 0) {
      capture_left_index_ = static_cast<long>(index);
    } else if (buffer.channelNum == 1) {
      capture_right_index_ = static_cast<long>(index);
    }
  }
  if (capture_left_index_ < 0 || capture_right_index_ < 0) {
    return;
  }

  ASIOChannelInfo left{};
  left.channel = 0;
  left.isInput = ASIOFalse;
  ASIOChannelInfo right{};
  right.channel = 1;
  right.isInput = ASIOFalse;
  ASIOSampleRate sample_rate = 0.0;
  if (backend_->getChannelInfo(&left) != ASE_OK ||
      backend_->getChannelInfo(&right) != ASE_OK ||
      backend_->getSampleRate(&sample_rate) != ASE_OK ||
      left.type != ASIOSTInt32LSB || right.type != ASIOSTInt32LSB ||
      sample_rate != 48000.0 ||
      !capture_publisher_.open(
          48000,
          static_cast<std::uint32_t>(buffer_size),
          capture_mapping_name_.data())) {
    capture_left_index_ = -1;
    capture_right_index_ = -1;
  }
}

void ProxyDriver::begin_callback(long buffer_index) noexcept {
  callback_buffer_index_ = buffer_index;
  capture_published_for_callback_ = false;
  in_callback_ = true;
}

void ProxyDriver::publish_capture() noexcept {
  capture_published_for_callback_ = true;
  if (!capture_publisher_.is_open() || callback_buffer_index_ < 0 ||
      callback_buffer_index_ > 1 || capture_left_index_ < 0 ||
      capture_right_index_ < 0) {
    return;
  }
  const auto* left = static_cast<const std::int32_t*>(
      downstream_buffers_[capture_left_index_].buffers[callback_buffer_index_]);
  const auto* right = static_cast<const std::int32_t*>(
      downstream_buffers_[capture_right_index_].buffers[callback_buffer_index_]);
  capture_publisher_.publish_int32_stereo(left, right);
}

void ProxyDriver::finish_callback() noexcept {
  if (!capture_published_for_callback_) {
    publish_capture();
  }
  in_callback_ = false;
}

void ProxyDriver::release_backend() noexcept {
  capture_publisher_.close();
  if (backend_ != nullptr) {
    backend_->Release();
    backend_ = nullptr;
  }
  state_ = State::created;
}

}  // namespace soundfork
