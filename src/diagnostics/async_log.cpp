#include "diagnostics/async_log.hpp"

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace soundfork::diagnostics {
namespace {
constexpr std::size_t kCapacity = 128;
struct Record {
  SYSTEMTIME time{};
  Level level = Level::info;
  char event[64]{};
  char detail[448]{};
};
template<std::size_t N>
void copy_text(char (&destination)[N], std::string_view text) noexcept {
  const auto length = (std::min)(text.size(), N - 1);
  for (std::size_t i = 0; i < length; ++i) {
    const auto character = text[i];
    destination[i] = character == '\r' || character == '\n' || character == '\t' ||
        character == '\0' ? ' ' : character;
  }
  destination[length] = '\0';
}
const wchar_t* filename(Component component) noexcept {
  switch (component) {
    case Component::control: return L"control.log";
    case Component::relay: return L"relay.log";
    case Component::hub: return L"hub.log";
  }
  return nullptr;
}
}  // namespace

struct AsyncLog::Impl {
  std::filesystem::path directory, path;
  std::string version;
  std::uint32_t limit = 0;
  std::array<Record, kCapacity> records{};
  std::size_t read = 0, count = 0;
  std::mutex mutex;
  std::condition_variable wake;
  bool stopping = false;
  std::thread worker;
  std::atomic<std::uint64_t> accepted{0}, written{0}, dropped{0}, io_errors{0};
  HANDLE file = INVALID_HANDLE_VALUE;
  std::uint64_t bytes = 0;
  ULONGLONG retry_at = 0;

  ~Impl() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
  void close_file() noexcept {
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
  }
  bool rotate() {
    close_file();
    const auto first = std::filesystem::path(path.native() + L".1");
    const auto second = std::filesystem::path(path.native() + L".2");
    if (!MoveFileExW(first.c_str(), second.c_str(), MOVEFILE_REPLACE_EXISTING) &&
        GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    if (!MoveFileExW(path.c_str(), first.c_str(), MOVEFILE_REPLACE_EXISTING) &&
        GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    bytes = 0;
    return true;
  }
  bool open_file() {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    file = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(file, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        information.nNumberOfLinks != 1 ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 0) {
      close_file();
      return false;
    }
    bytes = static_cast<std::uint64_t>(size.QuadPart);
    return true;
  }
  void append(const Record& record) noexcept {
    try {
      if (GetTickCount64() < retry_at) { ++io_errors; return; }
      if (file == INVALID_HANDLE_VALUE && !open_file()) { fail(); return; }
      char line[768]{};
      const char* level = record.level == Level::error ? "error" :
          record.level == Level::warning ? "warning" : "info";
      const int length = _snprintf_s(line, sizeof(line), _TRUNCATE,
          "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ level=%s pid=%lu version=%s event=%s %s\r\n",
          record.time.wYear, record.time.wMonth, record.time.wDay,
          record.time.wHour, record.time.wMinute, record.time.wSecond,
          record.time.wMilliseconds, level, GetCurrentProcessId(), version.c_str(),
          record.event, record.detail);
      if (length <= 0) { ++io_errors; return; }
      if (bytes + static_cast<std::uint64_t>(length) > limit) {
        if (!rotate() || !open_file()) { fail(); return; }
      }
      DWORD transferred = 0;
      if (!WriteFile(file, line, static_cast<DWORD>(length), &transferred, nullptr) ||
          transferred != static_cast<DWORD>(length)) { fail(); return; }
      bytes += transferred;
      ++written;
    } catch (...) { fail(); }
  }
  void fail() noexcept {
    ++io_errors;
    close_file();
    retry_at = GetTickCount64() + 1000;
  }
  void run() noexcept {
    try {
      std::uint64_t reported_drops = 0;
      for (;;) {
        std::array<Record, 8> batch{};
        std::size_t available = 0;
        bool done = false;
        {
          std::unique_lock guard(mutex);
          wake.wait_for(guard, std::chrono::seconds(1), [&] { return stopping || count != 0; });
          available = (std::min)(count, batch.size());
          for (std::size_t i = 0; i < available; ++i) {
            batch[i] = records[read];
            read = (read + 1) % kCapacity;
          }
          count -= available;
          done = stopping && count == 0;
        }
        for (std::size_t i = 0; i < available; ++i) append(batch[i]);
        const auto lost = dropped.load(std::memory_order_relaxed);
        if (lost != reported_drops) {
          Record summary;
          GetSystemTime(&summary.time);
          summary.level = Level::warning;
          copy_text(summary.event, "logger_dropped");
          _snprintf_s(summary.detail, sizeof(summary.detail), _TRUNCATE,
              "records=%llu", static_cast<unsigned long long>(lost));
          append(summary);
          reported_drops = lost;
        }
        if (done) break;
      }
    } catch (...) { ++io_errors; }
    close_file();
  }
};

AsyncLog::AsyncLog() noexcept = default;
AsyncLog::~AsyncLog() { stop(); }
bool AsyncLog::start(const std::filesystem::path& directory, Component component,
                     std::string_view version, std::uint32_t max_bytes) noexcept {
  if (impl_ || directory.empty() || filename(component) == nullptr ||
      version.empty() || version.size() > 64 || max_bytes < 1024 || max_bytes > 1024 * 1024)
    return false;
  try {
    auto state = std::make_unique<Impl>();
    state->directory = directory;
    state->path = directory / filename(component);
    char safe_version[65]{};
    copy_text(safe_version, version);
    state->version = safe_version;
    state->limit = max_bytes;
    impl_ = std::move(state);
    impl_->worker = std::thread([state = impl_.get()] { state->run(); });
    return true;
  } catch (...) { impl_.reset(); return false; }
}
void AsyncLog::write(Level level, std::string_view event, std::string_view detail) noexcept {
  if (!impl_) return;
  try {
    std::unique_lock guard(impl_->mutex, std::try_to_lock);
    if (!guard.owns_lock()) { ++impl_->dropped; return; }
    if (impl_->stopping || impl_->count == kCapacity) { ++impl_->dropped; return; }
    auto& record = impl_->records[(impl_->read + impl_->count) % kCapacity];
    GetSystemTime(&record.time);
    record.level = level;
    copy_text(record.event, event);
    copy_text(record.detail, detail);
    ++impl_->count;
    ++impl_->accepted;
    guard.unlock();
    impl_->wake.notify_one();
  } catch (...) { ++impl_->dropped; }
}
void AsyncLog::stop() noexcept {
  if (!impl_ || !impl_->worker.joinable()) return;
  {
    std::lock_guard guard(impl_->mutex);
    impl_->stopping = true;
  }
  impl_->wake.notify_one();
  impl_->worker.join();
}
LogStats AsyncLog::stats() const noexcept {
  if (!impl_) return {};
  return {impl_->accepted.load(), impl_->written.load(), impl_->dropped.load(), impl_->io_errors.load()};
}
std::filesystem::path log_directory() noexcept {
  PWSTR local = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return {};
  try {
    auto path = std::filesystem::path(local) / L"SoundFork" / L"Logs";
    CoTaskMemFree(local);
    return path;
  } catch (...) { CoTaskMemFree(local); return {}; }
}
AsyncLog& log() noexcept { static AsyncLog instance; return instance; }
void initialize_log(Component component, std::string_view version) noexcept {
  (void)log().start(log_directory(), component, version);
}
}  // namespace soundfork::diagnostics
