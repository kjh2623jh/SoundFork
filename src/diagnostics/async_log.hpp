#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace soundfork::diagnostics {

enum class Component { control, relay, hub };
enum class Level { info, warning, error };
struct LogStats {
  std::uint64_t accepted = 0, written = 0, dropped = 0, io_errors = 0;
};

// Not an ASIO-callback API. Only UI/capture-worker threads may enqueue logs.
// Producers never wait for the writer; congestion drops diagnostics, not audio.
class AsyncLog final {
 public:
  AsyncLog() noexcept;
  ~AsyncLog();
  AsyncLog(const AsyncLog&) = delete;
  AsyncLog& operator=(const AsyncLog&) = delete;
  bool start(const std::filesystem::path& directory, Component component,
             std::string_view version, std::uint32_t max_bytes = 1024 * 1024) noexcept;
  void write(Level level, std::string_view event, std::string_view detail = {}) noexcept;
  // Stop only after producers finish. Drains the queue; never call from audio callbacks.
  void stop() noexcept;
  LogStats stats() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::filesystem::path log_directory() noexcept;
AsyncLog& log() noexcept;
void initialize_log(Component component, std::string_view version) noexcept;

}  // namespace soundfork::diagnostics
