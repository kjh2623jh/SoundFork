// Exercise the real resource and dialog procedure offline. No GitHub request,
// elevation, installer execution, browser or production settings are used.
#include "../src/control-app/update_dialog.cpp"
#include <iostream>
#include <stdexcept>

using namespace soundfork::control;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
std::filesystem::path root;
std::atomic<int> downloads{0};
int launches = 0;
bool install_allowed = true;
bool cancelled_uac = false;
void fake_download(const UpdateResult& release, DownloadProgress& progress, DownloadedUpdate& output) noexcept {
  class Bytes final : public UpdateSource {
    std::size_t offset = 0;
    std::size_t read(std::span<std::byte> buffer) override {
      if (offset == 3) return 0;
      buffer[0] = static_cast<std::byte>("abc"[offset++]); return 1;
    }
  } source;
  download_update_from_source(release, source, progress, root / std::to_wstring(++downloads), output);
}
bool fake_launch(HWND, bool, const std::filesystem::path&) {
  ++launches;
  if (cancelled_uac) { SetLastError(ERROR_CANCELLED); return false; }
  return true;
}
bool fake_ready(HWND, bool) { return install_allowed; }
void finish_worker(HWND window, Dialog& state) {
  const auto until = GetTickCount64() + 5000;
  while (!state.done.load() && GetTickCount64() < until) Sleep(1);
  require(state.done.load(), "fake downloader stuck");
  SendMessageW(window, WM_TIMER, kDownloadTimer, 0);
  require(!state.worker.joinable(), "finished worker not joined");
}
std::wstring text(HWND window, int id) {
  std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(GetDlgItem(window, id))) + 1, L'\0');
  const int count = GetDlgItemTextW(window, id, value.data(), static_cast<int>(value.size()));
  value.resize(static_cast<std::size_t>(count)); return value;
}
}

int main() try {
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
  require(InitCommonControlsEx(&controls) != FALSE, "common controls initialization");
  root = std::filesystem::temp_directory_path() / (L"SoundForkUpdateUiTest-" + std::to_wstring(GetCurrentProcessId()));
  require(std::filesystem::create_directory(root), "test directory exists");
  struct Cleanup { ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); } } cleanup;
  UpdateResult release;
  release.latest_tag = L"v99.0.0";
  release.release_notes = L"## Changes\n\ud55c\uae00 \U0001f3b5\n- Test";
  release.installer_name = L"SoundFork-Setup-v99.0.0.exe";
  release.installer_url = L"https://github.com/kjh2623jh/SoundFork/releases/download/v99.0.0/SoundFork-Setup-v99.0.0.exe";
  release.installer_size = 3;
  release.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  for (const bool korean : {false, true}) {
    Dialog state{release, korean, fake_ready};
    state.download = fake_download; state.launch = fake_launch;
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), nullptr,
        update_proc, reinterpret_cast<LPARAM>(&state));
    if (!window) throw std::runtime_error("update resource did not load");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{window};
    require(text(window, IDC_UPDATE_NOTES).find(L"\r\n") != std::wstring::npos, "notes line breaks lost");
    require(text(window, IDC_UPDATE_NOTES).find(L"\ud55c\uae00") != std::wstring::npos, "Korean notes lost");
    require(text(window, IDC_UPDATE_ACTION) == (korean ? L"다운로드" : L"Download"), "download translation");
    RECT client{}; GetClientRect(window, &client);
    for (const int id : {IDC_UPDATE_NOTES, IDC_UPDATE_STATUS, IDC_UPDATE_PROGRESS, IDC_UPDATE_ACTION, IDCANCEL}) {
      RECT bounds{}; GetWindowRect(GetDlgItem(window, id), &bounds);
      MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&bounds), 2);
      require(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom,
          "update control clipped outside client area");
    }
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    finish_worker(window, state);
    require(state.downloaded.status == DownloadStatus::ready, "UI download not ready");
    const int before = launches;
    install_allowed = false;
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    require(launches == before && !state.result.installer_started, "installer launched despite active audio");
    install_allowed = true; cancelled_uac = true;
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    require(launches == before + 1 && !state.result.installer_started && state.downloaded.file, "UAC cancellation loses ready download");
    cancelled_uac = false;
    CheckDlgButton(window, IDC_UPDATE_DISABLE, BST_CHECKED);
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    require(launches == before + 2 && state.result.installer_started && state.result.disable_notifications,
        "verified install/notification choice not propagated");
  }
  {
    auto empty = release; empty.release_notes.clear(); empty.sha256.clear();
    Dialog state{empty, false, fake_ready};
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), nullptr,
        update_proc, reinterpret_cast<LPARAM>(&state));
    if (!window) throw std::runtime_error("empty notes dialog creation");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{window};
    require((GetWindowLongPtrW(GetDlgItem(window, IDC_UPDATE_NOTES), GWL_STYLE) & WS_VISIBLE) == 0 &&
        !IsWindowEnabled(GetDlgItem(window, IDC_UPDATE_ACTION)), "empty notes/unverifiable asset fallback");
    RECT client{}; GetClientRect(window, &client);
    RECT button{}; GetWindowRect(GetDlgItem(window, IDCANCEL), &button);
    MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&button), 2);
    require(button.top > 0 && button.bottom <= client.bottom, "compact update dialog clipped");
  }
  {
    // Corrupt files cannot reach even the fake launcher. Closing during an
    // outstanding worker requests cancellation and defers state destruction.
    Dialog state{release, false, fake_ready};
    state.download = fake_download; state.launch = fake_launch;
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), nullptr,
        update_proc, reinterpret_cast<LPARAM>(&state));
    if (!window) throw std::runtime_error("second update dialog creation");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{window};
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0); finish_worker(window, state);
    const int before = launches;
    release.sha256[0] = '0';
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    require(launches == before && !state.downloaded.file, "unverified installer launched");
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0); finish_worker(window, state);
    require(state.downloaded.status == DownloadStatus::failed && IsWindowEnabled(GetDlgItem(window, IDC_UPDATE_ACTION)), "failed download cannot retry");
    state.download = +[](const UpdateResult&, DownloadProgress& progress, DownloadedUpdate& output) noexcept {
      while (!progress.cancel.load()) Sleep(1);
      output.status = DownloadStatus::cancelled;
    };
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    SendMessageW(window, WM_CLOSE, 0, 0);
    require(state.closing && state.progress.cancel && !IsWindowEnabled(GetDlgItem(window, IDCANCEL)), "close did not defer cancellation");
    finish_worker(window, state);
  }
  std::cout << "Update dialog localization, layout, download, retry, cancellation, readiness and launch gating passed.\n";
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
