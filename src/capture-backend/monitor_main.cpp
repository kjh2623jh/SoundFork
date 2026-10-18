#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "capture-backend/shared_ring.hpp"

namespace {

void print_usage() {
  std::wcout << L"SoundFork capture monitor\n"
             << L"  soundfork_capture_monitor [--wait seconds] "
                L"[--duration seconds]\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  long wait_seconds = 30;
  long duration_seconds = 10;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--wait" && index + 1 < argc) {
      wait_seconds = std::wcstol(argv[++index], nullptr, 10);
    } else if (argument == L"--duration" && index + 1 < argc) {
      duration_seconds = std::wcstol(argv[++index], nullptr, 10);
    } else {
      print_usage();
      return 2;
    }
  }
  if (wait_seconds < 0 || duration_seconds <= 0) {
    print_usage();
    return 2;
  }

  soundfork::capture::SharedRingReader reader;
  const ULONGLONG wait_deadline =
      GetTickCount64() + static_cast<ULONGLONG>(wait_seconds) * 1000;
  while (!reader.open() && GetTickCount64() < wait_deadline) {
    Sleep(50);
  }
  if (!reader.is_open()) {
    std::wcerr << L"No active SoundFork capture stream was found\n";
    return 3;
  }

  const soundfork::capture::StreamFormat format = reader.format();
  std::wcout << L"Attached: " << format.sample_rate << L" Hz, "
             << format.channel_count << L" channels, "
             << format.frames_per_block << L" frames/block, Int32LSB\n";

  std::array<std::int32_t,
             soundfork::capture::kMaxFramesPerBlock *
                 soundfork::capture::kChannelCount>
      samples{};
  std::uint64_t blocks = 0;
  std::uint64_t frames = 0;
  std::int64_t peak = 0;
  const ULONGLONG run_deadline =
      GetTickCount64() + static_cast<ULONGLONG>(duration_seconds) * 1000;
  while (GetTickCount64() < run_deadline) {
    std::uint32_t frames_read = 0;
    if (!reader.read_int32_stereo(
            samples.data(), samples.size(), &frames_read)) {
      Sleep(1);
      continue;
    }
    ++blocks;
    frames += frames_read;
    const std::size_t sample_count =
        static_cast<std::size_t>(frames_read) * format.channel_count;
    for (std::size_t index = 0; index < sample_count; ++index) {
      const std::int64_t magnitude =
          samples[index] < 0 ? -static_cast<std::int64_t>(samples[index])
                             : static_cast<std::int64_t>(samples[index]);
      peak = (std::max)(peak, magnitude);
    }
  }

  const soundfork::capture::StreamStats stats = reader.stats();
  std::wcout << L"Consumed blocks: " << blocks << L"\nConsumed frames: "
             << frames << L"\nPeak magnitude: " << peak
             << L"\nProducer dropped blocks: " << stats.dropped_blocks << L'\n';
  return 0;
}
