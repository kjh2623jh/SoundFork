#include "control-app/obs_plugin_manager.hpp"

#include <shlobj.h>
#include <tlhelp32.h>

#include <array>
#include <utility>
#include <vector>

namespace soundfork::control::obs_plugin {
namespace {
namespace fs = std::filesystem;
constexpr std::array<const wchar_t*, 7> files{
    L"bin/64bit/soundfork-obs.dll", L"bin/64bit/soundfork_capture_hub.exe",
    L"data/locale/en-US.ini", L"data/locale/ko-KR.ini", L"LICENSE",
    L"THIRD_PARTY_NOTICES.md", L"THIRD_PARTY_NOTICES.en.md"};
constexpr std::array<const wchar_t*, 5> directories{
    L"bin/64bit", L"bin", L"data/locale", L"data", L""};

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
  Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&&) = delete;
  Handle(const Handle&) = delete;
};

bool absolute_plain_path(const fs::path& path) {
  if (!path.is_absolute() || path.root_name().native().size() != 2 ||
      path.root_name().native()[1] != L':' || path != path.lexically_normal()) return false;
  for (const auto& part : path.relative_path()) {
    const auto name = part.native();
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' ||
        name.back() == L' ' || name.find(L':') != std::wstring::npos) return false;
  }
  return true;
}

bool safe_existing_chain(const fs::path& path) {
  if (!absolute_plain_path(path)) return false;
  auto current = path.root_path();
  for (const auto& part : path.relative_path()) {
    current /= part;
    const DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const auto error = GetLastError();
      return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
  }
  return true;
}

bool regular_file(const fs::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool complete(const fs::path& root) {
  if (!safe_existing_chain(root)) return false;
  for (const auto* file : files) {
    if (!safe_existing_chain(root / file) || !regular_file(root / file)) return false;
  }
  return true;
}

// Hold directory handles without FILE_SHARE_DELETE. This prevents a junction
// or ancestor-directory swap between validation and the actual file operation.
Result lock_directories(const fs::path& path, bool create, std::vector<Handle>& locks) {
  if (!absolute_plain_path(path)) return {Error::unsafe_path};
  auto current = path.root_path();
  for (const auto& part : path.relative_path()) {
    current /= part;
    if (create && !CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
      return {Error::file_operation, GetLastError()};
    Handle handle(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) return {Error::file_operation, GetLastError()};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info)) return {Error::file_operation, GetLastError()};
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return {Error::unsafe_path};
    locks.push_back(std::move(handle));
  }
  return {};
}

bool delete_known_files(const fs::path& root) {
  bool success = true;
  for (const auto* file : files) {
    if (!DeleteFileW((root / file).c_str())) {
      const auto error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) success = false;
    }
  }
  return success;
}

Result transaction(bool install, const Paths& paths) {
  if (!absolute_plain_path(paths.installed) || paths.installed.filename() != L"soundfork-obs" ||
      !safe_existing_chain(paths.installed)) return {Error::unsafe_path};
  if (!install && GetFileAttributesW(paths.installed.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
  if (install && (!complete(paths.bundle) || file_version(paths.bundle / files[0]).empty()))
    return {Error::bundle_missing};

  std::vector<Handle> locks;
  std::vector<Handle> source_locks;
  if (install) {
    for (const auto* directory : {L"bin/64bit", L"data/locale"}) {
      const auto result = lock_directories(paths.bundle / directory, false, locks);
      if (result.error != Error::none) return result;
    }
    for (const auto* file : files) {
      Handle source(CreateFileW((paths.bundle / file).c_str(), GENERIC_READ,
          FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (source.value == INVALID_HANDLE_VALUE) return {Error::file_operation, GetLastError()};
      BY_HANDLE_FILE_INFORMATION info{};
      if (!GetFileInformationByHandle(source.value, &info)) return {Error::file_operation, GetLastError()};
      if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
        return {Error::unsafe_path};
      source_locks.push_back(std::move(source));
    }
  }
  for (const auto* directory : {L"bin/64bit", L"data/locale"}) {
    const auto result = lock_directories(paths.installed / directory, true, locks);
    if (result.error != Error::none) return result;
  }
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) return {Error::file_operation};
  std::array<wchar_t, 40> guid_text{};
  if (StringFromGUID2(guid, guid_text.data(), static_cast<int>(guid_text.size())) == 0)
    return {Error::file_operation};
  const auto stage = paths.installed.parent_path() / (L"soundfork-obs.transaction-" + std::wstring(guid_text.data()));
  if (!CreateDirectoryW(stage.c_str(), nullptr)) return {Error::file_operation, GetLastError()};
  for (const auto* branch : {L"new/bin/64bit", L"new/data/locale", L"old/bin/64bit", L"old/data/locale"}) {
    const auto result = lock_directories(stage / branch, true, locks);
    if (result.error != Error::none) return result;
  }
  auto cleanup = [&] {
    const bool first = delete_known_files(stage / L"new");
    const bool second = delete_known_files(stage / L"old");
    locks.clear();
    // Non-recursive removal leaves any unknown/user files untouched.
    for (const auto* branch : {L"new", L"old"}) {
      for (const auto* directory : directories) (void)RemoveDirectoryW((stage / branch / directory).c_str());
    }
    (void)RemoveDirectoryW(stage.c_str());
    if (!install) {
      for (const auto* directory : directories) (void)RemoveDirectoryW((paths.installed / directory).c_str());
    }
    return first && second;
  };

  std::array<bool, files.size()> existed{};
  std::array<bool, files.size()> applied{};
  std::array<fs::path, files.size()> destinations;
  std::array<fs::path, files.size()> replacements;
  std::array<fs::path, files.size()> backups;
  for (std::size_t index = 0; index < files.size(); ++index) {
    destinations[index] = paths.installed / files[index];
    replacements[index] = stage / L"new" / files[index];
    backups[index] = stage / L"old" / files[index];
  }
  Result result;
  // Prepare every replacement and backup before changing installed files.
  for (std::size_t index = 0; index < files.size(); ++index) {
    const auto& destination = destinations[index];
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        result = {Error::unsafe_path}; break;
      }
      existed[index] = true;
      if (install) {
        Handle previous(CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (previous.value == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(previous.value, &info)) {
          result = {Error::file_operation, GetLastError()}; break;
        }
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
          result = {Error::unsafe_path}; break;
        }
        if (!CopyFileW(destination.c_str(), backups[index].c_str(), TRUE)) {
          result = {Error::file_operation, GetLastError()}; break;
        }
      }
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
      result = {Error::file_operation, GetLastError()}; break;
    }
    if (install && !CopyFileW((paths.bundle / files[index]).c_str(), replacements[index].c_str(), TRUE)) {
      result = {Error::file_operation, GetLastError()}; break;
    }
  }
  if (result.error == Error::none) {
    for (std::size_t index = 0; index < files.size(); ++index) {
      if (!install && !existed[index]) continue;
      const auto& source = install ? replacements[index] : destinations[index];
      const auto& destination = install ? destinations[index] : backups[index];
      if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result = {Error::file_operation, GetLastError()}; break;
      }
      applied[index] = true;
    }
  }
  if (result.error != Error::none) {
    bool restored = true;
    for (std::size_t index = files.size(); index-- > 0;) {
      if (!applied[index]) continue;
      if (existed[index]) {
        if (!MoveFileExW(backups[index].c_str(), destinations[index].c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) restored = false;
      } else if (!DeleteFileW(destinations[index].c_str())) restored = false;
    }
    // Preserve recovery copies if rollback itself fails.
    if (!restored) return {Error::rollback_failed, GetLastError()};
  }
  if (!cleanup() && result.error == Error::none) return {Error::file_operation, GetLastError()};
  return result;
}
}  // namespace

std::wstring file_version(const fs::path& path) {
  DWORD ignored = 0;
  const auto bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (bytes == 0 || bytes > 1024 * 1024) return {};
  std::vector<std::byte> buffer(bytes);
  if (!GetFileVersionInfoW(path.c_str(), 0, bytes, buffer.data())) return {};
  void* value = nullptr;
  UINT length = 0;
  if (!VerQueryValueW(buffer.data(), L"\\StringFileInfo\\040904B0\\ProductVersion", &value, &length) ||
      value == nullptr || length == 0 || length > 128) return {};
  const auto* text = static_cast<const wchar_t*>(value);
  if (text[length - 1] != L'\0') return {};
  return std::wstring(text, length - 1);
}

Paths locate(const fs::path& executable_directory) {
  Paths paths;
  paths.bundle = executable_directory / L"obs-plugin/soundfork-obs";
  if (!complete(paths.bundle)) {
    const auto sibling = executable_directory.parent_path() / L"obs-plugin/soundfork-obs";
    if (complete(sibling)) paths.bundle = sibling;
  }
  PWSTR program_data = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &program_data))) {
    paths.installed = fs::path(program_data) / L"obs-studio/plugins/soundfork-obs";
    CoTaskMemFree(program_data);
  }
  return paths;
}

Status inspect(const Paths& paths) {
  Status status;
  if (paths.installed.empty()) return status;
  status.safe = safe_existing_chain(paths.installed);
  for (const auto* file : files) {
    if (GetFileAttributesW((paths.installed / file).c_str()) != INVALID_FILE_ATTRIBUTES) status.present = true;
  }
  status.complete = status.safe && complete(paths.installed);
  status.bundled = complete(paths.bundle);
  if (status.safe) status.installed_version = file_version(paths.installed / files[0]);
  if (status.bundled) status.bundled_version = file_version(paths.bundle / files[0]);
  status.bundled = status.bundled && !status.bundled_version.empty();
  status.update_required = status.present && status.bundled &&
      (!status.complete || status.installed_version != status.bundled_version);
  return status;
}

bool obs_is_running() noexcept {
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot.value == INVALID_HANDLE_VALUE) return true;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.value, &entry)) return true;
  do {
    if (_wcsicmp(entry.szExeFile, L"obs64.exe") == 0 || _wcsicmp(entry.szExeFile, L"obs32.exe") == 0) return true;
  } while (Process32NextW(snapshot.value, &entry));
  if (GetLastError() != ERROR_NO_MORE_FILES) return true;
  // Keep the legacy lease check: an old libobs host must not have its DLL
  // replaced either, even when its executable is not named obs64.exe.
  for (const auto* name : {L"Local\\SoundFork.CaptureHub.v1.OBSClient", L"Local\\SoundFork.CaptureHub.v2.OBSClient"}) {
    HANDLE lease = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (lease != nullptr) { CloseHandle(lease); return true; }
    if (GetLastError() == ERROR_ACCESS_DENIED) return true;
  }
  return false;
}

Result transact_files(bool install, const Paths& paths) {
  try { return transaction(install, paths); }
  catch (...) { return {Error::file_operation, ERROR_UNHANDLED_EXCEPTION}; }
}

Result manage(bool install, const Paths& paths) {
  Handle mutex(CreateMutexW(nullptr, FALSE, L"Global\\SoundFork.ObsPluginManagement.v1"));
  if (mutex.value == nullptr || mutex.value == INVALID_HANDLE_VALUE) return {Error::file_operation, GetLastError()};
  const auto acquired = WaitForSingleObject(mutex.value, 0);
  if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) return {Error::busy};
  struct Release { HANDLE value; ~Release() { ReleaseMutex(value); } } release{mutex.value};
  if (!install && !inspect(paths).present) return {};
  if (obs_is_running()) return {Error::obs_running};
  return transact_files(install, paths);
}
}  // namespace soundfork::control::obs_plugin
