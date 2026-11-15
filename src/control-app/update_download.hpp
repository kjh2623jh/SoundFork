#pragma once

#include "control-app/update_checker.hpp"
#include "platform/unique_handle.hpp"

#include <atomic>
#include <filesystem>
#include <span>

namespace soundfork::control {
enum class DownloadStatus { failed, cancelled, ready };
struct DownloadProgress {
  std::atomic<bool> cancel{false};
  std::atomic<std::uint64_t> received{0};
};

// read() returns bytes, zero for EOF; throws on transport errors. The same
// verification/file path is used by HTTPS and deterministic offline tests.
class UpdateSource {
 public:
  virtual ~UpdateSource() = default;
  virtual std::size_t read(std::span<std::byte> buffer) = 0;
};

class DownloadedUpdate final {
 public:
  ~DownloadedUpdate();
  DownloadedUpdate() = default;
  DownloadedUpdate(const DownloadedUpdate&) = delete;
  DownloadedUpdate& operator=(const DownloadedUpdate&) = delete;
  DownloadStatus status = DownloadStatus::failed;
  std::filesystem::path path;
  // Retain the file once Windows has started the installer. Until then,
  // destruction deletes only the file and directory this object created.
  void retain() noexcept { retained_ = true; }
  void reset() noexcept;
  platform::UniqueHandle file;
 private:
  bool retained_ = false;
};

void download_update(const UpdateResult&, DownloadProgress&, DownloadedUpdate&) noexcept;
void download_update_from_source(const UpdateResult&, UpdateSource&, DownloadProgress&,
    const std::filesystem::path& directory, DownloadedUpdate&) noexcept;
// Re-read the held, non-write-shared file immediately before launch.
bool verify_download(const UpdateResult&, const DownloadedUpdate&) noexcept;
bool trusted_update_redirect(std::wstring_view url) noexcept;
}  // namespace soundfork::control
