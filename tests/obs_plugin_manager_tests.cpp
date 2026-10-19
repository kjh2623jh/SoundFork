#include <windows.h>
#include <objbase.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "control-app/obs_plugin_manager.hpp"

namespace {
namespace fs = std::filesystem;
namespace plugin = soundfork::control::obs_plugin;
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
  require(file.good(), "fixture write failed");
}
std::string read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct TemporaryDirectory {
  fs::path root;
  fs::path parent;
  TemporaryDirectory() {
    std::vector<wchar_t> temp(32768, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    require(length > 0 && length < temp.size(), "temporary path unavailable");
    parent = fs::path(temp.data()).lexically_normal();
    if (parent.filename().empty()) parent = parent.parent_path();
    GUID guid{};
    require(SUCCEEDED(CoCreateGuid(&guid)), "test GUID failed");
    std::array<wchar_t, 40> text{};
    require(StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) != 0, "GUID formatting failed");
    root = parent / (L"SoundForkObsManagerTest-" + std::wstring(text.data()));
    require(fs::create_directory(root), "test directory creation failed");
  }
  ~TemporaryDirectory() {
    // Only delete the exact, newly created test directory under system temp.
    if (root.is_absolute() && root.parent_path() == parent &&
        root.filename().native().starts_with(L"SoundForkObsManagerTest-")) {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  }
};

void exercise(const fs::path& fixture) {
  TemporaryDirectory temporary;
  plugin::Paths paths{temporary.root / L"app/obs-plugin/soundfork-obs",
                       temporary.root / L"plugins/soundfork-obs"};
  const auto dll = fs::path(L"bin/64bit/soundfork-obs.dll");
  const auto hub = fs::path(L"bin/64bit/soundfork_capture_hub.exe");
  fs::create_directories((paths.bundle / dll).parent_path());
  fs::copy_file(fixture, paths.bundle / dll);
  write(paths.bundle / hub, "original hub");
  for (const auto* file : {L"data/locale/en-US.ini", L"data/locale/ko-KR.ini", L"LICENSE",
                          L"THIRD_PARTY_NOTICES.md", L"THIRD_PARTY_NOTICES.en.md"}) {
    write(paths.bundle / file, "fixture asset");
  }
  require(!plugin::file_version(fixture).empty(), "version resource not read");
  const auto initial = plugin::inspect(paths);
  require(initial.safe && initial.bundled && !initial.present, "fresh status incorrect");
  require(plugin::locate(temporary.root / L"app").bundle == paths.bundle, "installed payload lookup failed");
  require(plugin::locate(temporary.root / L"app/bin").bundle == paths.bundle, "ZIP payload lookup failed");

  HANDLE lease = CreateEventW(nullptr, TRUE, FALSE, L"Local\\SoundFork.CaptureHub.v1.OBSClient");
  if (lease == nullptr) throw std::runtime_error("OBS lease fixture failed");
  const auto blocked = plugin::manage(true, paths);
  CloseHandle(lease);
  require(blocked.error == plugin::Error::obs_running && !fs::exists(paths.installed),
          "active OBS capture did not block installation");

  require(plugin::transact_files(true, paths).error == plugin::Error::none, "install failed");
  const auto installed = plugin::inspect(paths);
  require(installed.complete && installed.installed_version == installed.bundled_version, "installed version incorrect");
  const auto original_dll_size = fs::file_size(paths.installed / dll);
  write(paths.installed / L"user-notes.txt", "keep my file");
  write(temporary.root / L"plugins/other-plugin/keep.txt", "keep other plugin");

  write(paths.bundle / hub, "updated hub");
  { std::ofstream append(paths.bundle / dll, std::ios::binary | std::ios::app); append << "new build"; }
  HANDLE locked = CreateFileW((paths.installed / hub).c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (locked == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot lock installed hub fixture");
  const auto failed_update = plugin::transact_files(true, paths);
  const auto failed_remove = plugin::transact_files(false, paths);
  CloseHandle(locked);
  require(failed_update.error == plugin::Error::file_operation, "locked update was not rejected");
  require(failed_remove.error == plugin::Error::file_operation, "locked removal was not rejected");
  require(fs::file_size(paths.installed / dll) == original_dll_size &&
          read(paths.installed / hub) == "original hub", "failed operation did not roll back earlier files");
  require(plugin::transact_files(true, paths).error == plugin::Error::none, "update failed");
  require(fs::file_size(paths.installed / dll) > original_dll_size &&
          read(paths.installed / hub) == "updated hub", "update did not replace payload");

  fs::remove(paths.bundle / L"data/locale/ko-KR.ini");
  require(plugin::transact_files(true, paths).error == plugin::Error::bundle_missing, "incomplete payload accepted");
  require(read(paths.installed / hub) == "updated hub", "missing payload damaged installation");
  write(paths.bundle / L"data/locale/ko-KR.ini", "restored asset");
  const auto invalid_destination = paths.installed / L"data/locale/en-US.ini";
  fs::remove(invalid_destination);
  fs::create_directory(invalid_destination);
  require(plugin::transact_files(true, paths).error == plugin::Error::unsafe_path, "unexpected directory accepted as a file");
  fs::remove(invalid_destination);
  write(invalid_destination, "restored asset");

  auto invalid = paths;
  invalid.installed = L"relative/soundfork-obs";
  require(plugin::transact_files(false, invalid).error == plugin::Error::unsafe_path, "relative destination accepted");
  invalid.installed = temporary.root;
  require(plugin::transact_files(false, invalid).error == plugin::Error::unsafe_path, "broad destination accepted");

  // Directory symlink tests are conditional on Windows Developer Mode/admin
  // support; the regular install/rollback tests require neither elevation nor OBS.
  const auto link_parent = temporary.root / L"linked";
  fs::create_directory(link_parent);
  const auto link = link_parent / L"soundfork-obs";
  if (CreateSymbolicLinkW(link.c_str(), paths.installed.c_str(),
                         SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    invalid.installed = link;
    const auto rejected = plugin::transact_files(false, invalid);
    RemoveDirectoryW(link.c_str());
    require(rejected.error == plugin::Error::unsafe_path, "linked destination accepted");
  } else {
    std::cout << "Directory symlink test skipped (Windows privilege unavailable)\n";
  }
  require(plugin::transact_files(false, paths).error == plugin::Error::none, "remove failed");
  require(!plugin::inspect(paths).present && !fs::exists(paths.installed / dll), "known plugin files remain");
  require(read(paths.installed / L"user-notes.txt") == "keep my file", "unknown user file removed");
  require(read(temporary.root / L"plugins/other-plugin/keep.txt") == "keep other plugin", "other plugin changed");
  require(plugin::transact_files(false, paths).error == plugin::Error::none, "repeated removal failed");
  for (const auto& entry : fs::directory_iterator(paths.installed.parent_path())) {
    require(!entry.path().filename().native().starts_with(L"soundfork-obs.transaction-"), "transaction files leaked");
  }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) return 2;
  struct ComCloser { ~ComCloser() { CoUninitialize(); } } closer;
  try {
    require(argc == 2, "expected versioned control executable fixture");
    exercise(argv[1]);
    std::cout << "OBS manager: version, install, update, rollback, remove, busy/path guards and user-file preservation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
