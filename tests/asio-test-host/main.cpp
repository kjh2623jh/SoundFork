#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "audio-bridge/backend_factory.hpp"
#include "audio-bridge/proxy_driver.hpp"

namespace {

enum class TestSignal {
  silence,
  sine_stereo,
  sine_left,
  sine_right,
};

struct DriverRecord {
  std::wstring key_name;
  std::wstring description;
  CLSID clsid{};
};

struct HostContext {
  IASIO* driver = nullptr;
  ASIOBufferInfo buffers[2]{};
  ASIOChannelInfo channels[2]{};
  long buffer_size = 0;
  double phase = 0.0;
  double phase_step = 0.0;
  double gain_linear = 0.01;
  TestSignal signal = TestSignal::silence;
  std::atomic<std::uint64_t> callbacks{0};
  std::atomic<bool> reset_requested{false};
};

HostContext* g_host = nullptr;

bool read_string(HKEY key, const wchar_t* name, std::wstring* value) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      type != REG_SZ || bytes < sizeof(wchar_t)) {
    return false;
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
  if (RegQueryValueExW(
          key,
          name,
          nullptr,
          &type,
          reinterpret_cast<BYTE*>(buffer.data()),
          &bytes) != ERROR_SUCCESS) {
    return false;
  }
  *value = buffer.data();
  return true;
}

std::vector<DriverRecord> enumerate_drivers() {
  std::vector<DriverRecord> result;
  HKEY asio_key = nullptr;
  if (RegOpenKeyExW(
          HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\ASIO",
          0,
          KEY_READ | KEY_WOW64_64KEY,
          &asio_key) != ERROR_SUCCESS) {
    return result;
  }

  for (DWORD index = 0;; ++index) {
    std::array<wchar_t, 256> key_name{};
    DWORD key_length = static_cast<DWORD>(key_name.size());
    const LSTATUS enum_status = RegEnumKeyExW(
        asio_key, index, key_name.data(), &key_length, nullptr, nullptr, nullptr, nullptr);
    if (enum_status == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (enum_status != ERROR_SUCCESS) {
      continue;
    }

    HKEY driver_key = nullptr;
    if (RegOpenKeyExW(
            asio_key, key_name.data(), 0, KEY_READ | KEY_WOW64_64KEY, &driver_key) !=
        ERROR_SUCCESS) {
      continue;
    }
    std::wstring clsid_text;
    std::wstring description;
    if (read_string(driver_key, L"CLSID", &clsid_text)) {
      DriverRecord record;
      record.key_name = key_name.data();
      record.description = read_string(driver_key, L"Description", &description)
                               ? description
                               : record.key_name;
      if (SUCCEEDED(CLSIDFromString(clsid_text.c_str(), &record.clsid))) {
        result.push_back(std::move(record));
      }
    }
    RegCloseKey(driver_key);
  }
  RegCloseKey(asio_key);
  return result;
}

int bytes_per_sample(ASIOSampleType type) {
  switch (type) {
    case ASIOSTInt16LSB:
    case ASIOSTInt16MSB:
      return 2;
    case ASIOSTInt24LSB:
    case ASIOSTInt24MSB:
      return 3;
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    case ASIOSTFloat32LSB:
    case ASIOSTInt32MSB:
    case ASIOSTInt32MSB16:
    case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20:
    case ASIOSTInt32MSB24:
    case ASIOSTFloat32MSB:
      return 4;
    case ASIOSTFloat64LSB:
    case ASIOSTFloat64MSB:
      return 8;
    default:
      return 0;
  }
}

bool can_write_sine(ASIOSampleType type) {
  return type == ASIOSTInt16LSB || type == ASIOSTInt24LSB ||
         type == ASIOSTInt32LSB || type == ASIOSTFloat32LSB ||
         type == ASIOSTFloat64LSB;
}

void write_little_endian_sample(
    void* buffer, long frame, ASIOSampleType type, double value) noexcept {
  switch (type) {
    case ASIOSTInt16LSB:
      static_cast<std::int16_t*>(buffer)[frame] =
          static_cast<std::int16_t>(value * 32767.0);
      break;
    case ASIOSTInt24LSB: {
      const std::int32_t sample = static_cast<std::int32_t>(value * 8388607.0);
      auto* bytes = static_cast<std::uint8_t*>(buffer) + frame * 3;
      bytes[0] = static_cast<std::uint8_t>(sample);
      bytes[1] = static_cast<std::uint8_t>(sample >> 8);
      bytes[2] = static_cast<std::uint8_t>(sample >> 16);
      break;
    }
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
      static_cast<std::int32_t*>(buffer)[frame] =
          static_cast<std::int32_t>(value * 2147483647.0);
      break;
    case ASIOSTFloat32LSB:
      static_cast<float*>(buffer)[frame] = static_cast<float>(value);
      break;
    case ASIOSTFloat64LSB:
      static_cast<double*>(buffer)[frame] = value;
      break;
    default:
      break;
  }
}

void process_buffer(long index) noexcept {
  HostContext& host = *g_host;
  if (host.signal == TestSignal::silence) {
    for (long channel = 0; channel < 2; ++channel) {
      const int width = bytes_per_sample(host.channels[channel].type);
      if (width > 0) {
        std::memset(
            host.buffers[channel].buffers[index],
            0,
            static_cast<std::size_t>(host.buffer_size * width));
      }
    }
  } else {
    constexpr double kTwoPi = 6.28318530717958647692;
    for (long frame = 0; frame < host.buffer_size; ++frame) {
      const double value = std::sin(host.phase) * host.gain_linear;
      host.phase += host.phase_step;
      if (host.phase >= kTwoPi) {
        host.phase -= kTwoPi;
      }
      for (long channel = 0; channel < 2; ++channel) {
        const bool enabled = host.signal == TestSignal::sine_stereo ||
                             (host.signal == TestSignal::sine_left && channel == 0) ||
                             (host.signal == TestSignal::sine_right && channel == 1);
        write_little_endian_sample(
            host.buffers[channel].buffers[index],
            frame,
            host.channels[channel].type,
            enabled ? value : 0.0);
      }
    }
  }
  host.callbacks.fetch_add(1, std::memory_order_relaxed);
  host.driver->outputReady();
}

void buffer_switch(long index, ASIOBool) {
  process_buffer(index);
}

void sample_rate_changed(ASIOSampleRate) {}

long asio_message(long selector, long value, void*, double*) {
  if (selector == kAsioSelectorSupported) {
    return value == kAsioResetRequest || value == kAsioResyncRequest ||
                   value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo
               ? 1
               : 0;
  }
  if (selector == kAsioSupportsTimeInfo) {
    return 1;
  }
  if (selector == kAsioResetRequest || selector == kAsioResyncRequest) {
    g_host->reset_requested.store(true, std::memory_order_relaxed);
    return 1;
  }
  return selector == kAsioEngineVersion ? 2 : 0;
}

ASIOTime* buffer_switch_time_info(ASIOTime* time_info, long index, ASIOBool) {
  process_buffer(index);
  return time_info;
}

void print_usage() {
  std::wcout
      << L"SoundFork ASIO test host\n"
      << L"  soundfork_host --list\n"
      << L"  soundfork_host --driver <name> [--sample-rate 48000] "
         L"[--buffer-size 64] [--channels 0,1] [--duration seconds] "
         L"[--signal silence|sine|sine-left|sine-right] [--gain-db -40]\n"
      << L"  soundfork_host --proxy-backend <name> [same options]\n";
}

const wchar_t* sample_type_name(ASIOSampleType type) {
  switch (type) {
    case ASIOSTInt16LSB:
      return L"Int16LSB";
    case ASIOSTInt24LSB:
      return L"Int24LSB";
    case ASIOSTInt32LSB:
      return L"Int32LSB";
    case ASIOSTInt32LSB24:
      return L"Int32LSB24";
    case ASIOSTFloat32LSB:
      return L"Float32LSB";
    case ASIOSTFloat64LSB:
      return L"Float64LSB";
    default:
      return L"Other";
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) {
    std::wcerr << L"COM initialization failed: 0x" << std::hex << com_result << L'\n';
    return 2;
  }

  const auto drivers = enumerate_drivers();
  if (argc == 2 && std::wstring_view(argv[1]) == L"--list") {
    for (const DriverRecord& driver : drivers) {
      std::wcout << driver.key_name << L" - " << driver.description << L'\n';
    }
    CoUninitialize();
    return 0;
  }

  std::wstring driver_name;
  bool use_proxy = false;
  double sample_rate = 48000.0;
  long buffer_size = 64;
  long duration_seconds = 0;
  double gain_db = -40.0;
  TestSignal signal = TestSignal::silence;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--driver" && index + 1 < argc) {
      driver_name = argv[++index];
    } else if (argument == L"--proxy-backend" && index + 1 < argc) {
      driver_name = argv[++index];
      use_proxy = true;
    } else if (argument == L"--sample-rate" && index + 1 < argc) {
      sample_rate = std::wcstod(argv[++index], nullptr);
    } else if (argument == L"--buffer-size" && index + 1 < argc) {
      buffer_size = std::wcstol(argv[++index], nullptr, 10);
    } else if (argument == L"--duration" && index + 1 < argc) {
      duration_seconds = std::wcstol(argv[++index], nullptr, 10);
    } else if (argument == L"--signal" && index + 1 < argc) {
      const std::wstring_view value(argv[++index]);
      if (value == L"silence") {
        signal = TestSignal::silence;
      } else if (value == L"sine") {
        signal = TestSignal::sine_stereo;
      } else if (value == L"sine-left") {
        signal = TestSignal::sine_left;
      } else if (value == L"sine-right") {
        signal = TestSignal::sine_right;
      } else {
        std::wcerr << L"Unknown signal: " << value << L'\n';
        CoUninitialize();
        return 2;
      }
    } else if (argument == L"--gain-db" && index + 1 < argc) {
      gain_db = std::wcstod(argv[++index], nullptr);
      if (!std::isfinite(gain_db) || gain_db < -120.0 || gain_db > 0.0) {
        std::wcerr << L"--gain-db must be between -120 and 0 dBFS\n";
        CoUninitialize();
        return 2;
      }
    } else if (argument == L"--channels" && index + 1 < argc) {
      if (std::wstring_view(argv[++index]) != L"0,1") {
        std::wcerr << L"Phase 1 host supports --channels 0,1 only\n";
        CoUninitialize();
        return 2;
      }
    } else {
      print_usage();
      CoUninitialize();
      return 2;
    }
  }

  if (driver_name.empty()) {
    print_usage();
    CoUninitialize();
    return 2;
  }

  const auto selected = std::find_if(
      drivers.begin(), drivers.end(), [&](const DriverRecord& candidate) {
        return _wcsicmp(candidate.key_name.c_str(), driver_name.c_str()) == 0 ||
               _wcsicmp(candidate.description.c_str(), driver_name.c_str()) == 0;
      });
  if (selected == drivers.end()) {
    std::wcerr << L"ASIO driver not found: " << driver_name << L'\n';
    CoUninitialize();
    return 3;
  }

  IASIO* driver = nullptr;
  std::unique_ptr<soundfork::ClsidBackendFactory> proxy_factory;
  HRESULT result = S_OK;
  if (use_proxy) {
    proxy_factory =
        std::make_unique<soundfork::ClsidBackendFactory>(selected->clsid);
    driver = new (std::nothrow) soundfork::ProxyDriver(*proxy_factory);
    if (driver == nullptr) {
      result = E_OUTOFMEMORY;
    }
  } else {
    result = CoCreateInstance(
        selected->clsid,
        nullptr,
        CLSCTX_INPROC_SERVER,
        selected->clsid,
        reinterpret_cast<void**>(&driver));
  }
  if (FAILED(result) || driver == nullptr) {
    std::wcerr << L"Driver activation failed: 0x" << std::hex << result << L'\n';
    CoUninitialize();
    return 4;
  }

  int exit_code = 0;
  if (driver->init(GetConsoleWindow()) != ASIOTrue) {
    char error[124]{};
    driver->getErrorMessage(error);
    std::cerr << "Driver initialization failed: " << error << '\n';
    exit_code = 5;
  } else {
    long input_count = 0;
    long output_count = 0;
    long min_size = 0;
    long max_size = 0;
    long preferred_size = 0;
    long granularity = 0;
    long input_latency = 0;
    long output_latency = 0;
    ASIOSampleRate current_rate = 0.0;
    driver->getChannels(&input_count, &output_count);
    driver->getBufferSize(&min_size, &max_size, &preferred_size, &granularity);
    driver->getSampleRate(&current_rate);
    driver->getLatencies(&input_latency, &output_latency);
    std::wcout << L"Path: " << (use_proxy ? L"SoundFork proxy" : L"direct")
               << L" -> " << selected->description << L'\n';
    std::wcout << L"Inputs: " << input_count << L", outputs: " << output_count
               << L"\nBuffer: min=" << min_size << L", max=" << max_size
               << L", preferred=" << preferred_size << L", granularity="
               << granularity << L"\nInitial sample rate: " << current_rate << L" Hz"
               << L"\nInitial reported latency: input=" << input_latency
               << L" samples, output=" << output_latency << L" samples\n";

    if (duration_seconds > 0) {
      if (output_count < 2 || driver->canSampleRate(sample_rate) != ASE_OK) {
        std::wcerr << L"Requested stereo/sample-rate configuration is unsupported\n";
        exit_code = 6;
      } else {
        if (current_rate != sample_rate && driver->setSampleRate(sample_rate) != ASE_OK) {
          std::wcerr << L"Failed to set sample rate\n";
          exit_code = 6;
        } else {
          HostContext host;
          host.driver = driver;
          host.buffer_size = buffer_size;
          host.signal = signal;
          host.gain_linear = std::pow(10.0, gain_db / 20.0);
          host.phase_step = 6.28318530717958647692 * 440.0 / sample_rate;
          for (long channel = 0; channel < 2; ++channel) {
            host.buffers[channel].isInput = ASIOFalse;
            host.buffers[channel].channelNum = channel;
            host.channels[channel].channel = channel;
            host.channels[channel].isInput = ASIOFalse;
            if (driver->getChannelInfo(&host.channels[channel]) != ASE_OK) {
              exit_code = 7;
            }
            std::wcout << L"Output " << channel << L": "
                       << sample_type_name(host.channels[channel].type) << L'\n';
          }

          if (signal != TestSignal::silence &&
              (!can_write_sine(host.channels[0].type) ||
               !can_write_sine(host.channels[1].type))) {
            std::wcerr << L"Sine generation does not support this sample type\n";
            exit_code = 7;
          }

          ASIOCallbacks host_callbacks{};
          host_callbacks.bufferSwitch = &buffer_switch;
          host_callbacks.sampleRateDidChange = &sample_rate_changed;
          host_callbacks.asioMessage = &asio_message;
          host_callbacks.bufferSwitchTimeInfo = &buffer_switch_time_info;
          g_host = &host;
          if (exit_code == 0 &&
              driver->createBuffers(host.buffers, 2, buffer_size, &host_callbacks) ==
                  ASE_OK) {
            ASIOSampleRate effective_rate = 0.0;
            long active_input_latency = 0;
            long active_output_latency = 0;
            const ASIOError rate_result = driver->getSampleRate(&effective_rate);
            driver->getLatencies(&active_input_latency, &active_output_latency);
            if ((rate_result == ASE_OK || rate_result == ASE_SUCCESS) &&
                effective_rate > 0.0) {
              std::wcout << L"Active sample rate: " << effective_rate << L" Hz"
                         << L"\nActive buffer duration: "
                         << (1000.0 * static_cast<double>(buffer_size) /
                             effective_rate)
                         << L" ms"
                         << L"\nActive reported latency: input="
                         << active_input_latency << L" samples, output="
                         << active_output_latency << L" samples ("
                         << (1000.0 * static_cast<double>(active_output_latency) /
                             effective_rate)
                         << L" ms)\n";
            } else {
              std::wcout << L"Active sample rate unavailable\n"
                         << L"Active reported latency: input="
                         << active_input_latency << L" samples, output="
                         << active_output_latency << L" samples\n";
            }
            if (driver->start() == ASE_OK) {
              Sleep(static_cast<DWORD>(duration_seconds * 1000));
              driver->stop();
              std::wcout << L"Callbacks: " << host.callbacks.load() << L'\n';
              if (host.reset_requested.load()) {
                std::wcout << L"Driver requested reset/resync during the run\n";
              }
            } else {
              std::wcerr << L"Driver start failed\n";
              exit_code = 8;
            }
            driver->disposeBuffers();
          } else if (exit_code == 0) {
            std::wcerr << L"Buffer creation failed\n";
            exit_code = 8;
          }
          g_host = nullptr;
        }
      }
    }
  }

  driver->Release();
  CoUninitialize();
  return exit_code;
}
