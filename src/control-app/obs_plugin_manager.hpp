#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace soundfork::control::obs_plugin {

enum class Error : DWORD {
  none = 0,
  obs_running = 20,
  bundle_missing = 21,
  unsafe_path = 22,
  file_operation = 23,
  rollback_failed = 24,
  busy = 25,
};

struct Paths {
  std::filesystem::path bundle;
  std::filesystem::path installed;
};

struct Status {
  bool present = false;
  bool complete = false;
  bool bundled = false;
  bool safe = false;
  std::wstring installed_version;
  std::wstring bundled_version;
};

struct Result {
  Error error = Error::none;
  DWORD system_error = ERROR_SUCCESS;
};

Paths locate(const std::filesystem::path& executable_directory);
Status inspect(const Paths& paths);
std::wstring file_version(const std::filesystem::path& path);
bool obs_is_running() noexcept;
Result manage(bool install, const Paths& paths);

// File-only transaction, also used by isolated temporary-directory tests.
// The elevated command never accepts paths and calls manage(locate(...)).
Result transact_files(bool install, const Paths& paths);

}  // namespace soundfork::control::obs_plugin
