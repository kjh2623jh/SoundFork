// Exercise the real resource and dialog procedure offline. No GitHub request,
// elevation, installer execution, browser or production settings are used.
#include "../src/control-app/update_dialog.cpp"
#include <commdlg.h>
#include <iostream>
#include <stdexcept>

using namespace soundfork::control;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
std::filesystem::path root;
std::atomic<int> downloads{0};
int launches = 0;
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
  require(selected_release_notes(L"before<!-- soundfork-notes:start -->\n## Notes\n<!-- soundfork-notes:end -->after") == L"## Notes", "marked notes selection");
  require(selected_release_notes(L"unmarked") == L"unmarked", "unmarked notes fallback");
  require(selected_release_notes(L"<!-- soundfork-notes:start -->unfinished").find(L"unfinished") != std::wstring_view::npos, "incomplete marker fallback");
  const auto formatted = release_notes_rtf(L"## Heading\n- **Bold** `code` {\\object}");
  require(formatted.find("## Heading") == std::string::npos && formatted.find("{\\b Bold}") != std::string::npos &&
      formatted.find("\\u8226?") != std::string::npos && formatted.find("\\{\\\\object\\}") != std::string::npos, "Markdown formatting/RTF escaping");
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
  require(InitCommonControlsEx(&controls) != FALSE, "common controls initialization");
  root = std::filesystem::temp_directory_path() / (L"SoundForkUpdateUiTest-" + std::to_wstring(GetCurrentProcessId()));
  require(std::filesystem::create_directory(root), "test directory exists");
  struct Cleanup { ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); } } cleanup;
  UpdateResult release;
  release.latest_tag = L"v99.0.0";
  release.release_notes = L"Excluded<!-- soundfork-notes:start -->\n## Changes\n\ud55c\uae00 \U0001f3b5\n- **Bold** `code`\n<!-- soundfork-notes:end -->Hidden";
  release.installer_name = L"SoundFork-Setup-v99.0.0.exe";
  release.installer_url = L"https://github.com/kjh2623jh/SoundFork/releases/download/v99.0.0/SoundFork-Setup-v99.0.0.exe";
  release.installer_size = 3;
  const auto end_marker = release.release_notes.find(L"<!-- soundfork-notes:end -->");
  for (int line = 0; line < 50; ++line) release.release_notes.insert(end_marker, L"- Additional changes\n");
  release.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  {
    Dialog state{release, true};
    const auto modal = +[](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> INT_PTR {
      if (message == WM_APP) {
        RECT compact{};
        GetClientRect(window, &compact);
        const HWND notes = GetDlgItem(window, IDC_UPDATE_NOTES);
        const auto notes_visible = [notes] { return (GetWindowLongPtrW(notes, GWL_STYLE) & WS_VISIBLE) != 0; };
        const bool starts_collapsed = !notes_visible() &&
            GetFocus() == GetDlgItem(window, IDCANCEL);
        SendMessageW(window, WM_COMMAND, IDC_UPDATE_TOGGLE_NOTES, 0);
        RECT expanded{};
        GetClientRect(window, &expanded);
        const bool opened = notes_visible() && expanded.bottom > compact.bottom;
        CHARRANGE selected{};
        SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selected));
        const bool at_top = SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_GETFIRSTVISIBLELINE, 0, 0) == 0;
        const bool no_selection = selected.cpMin == 0 && selected.cpMax == 0;
        SendMessageW(window, WM_COMMAND, IDC_UPDATE_TOGGLE_NOTES, 0);
        RECT collapsed{};
        GetClientRect(window, &collapsed);
        const bool closed = !notes_visible() && collapsed.bottom == compact.bottom;
        if (!(starts_collapsed && opened && at_top && no_selection && closed)) {
          std::cerr << "starts collapsed=" << starts_collapsed << ", opened=" << opened
                    << ", at top=" << at_top << ", no selection=" << no_selection << ", closed=" << closed << '\n';
        }
        EndDialog(window, starts_collapsed && opened && at_top && no_selection && closed ? IDOK : IDCANCEL);
        return TRUE;
      }
      const auto handled = update_proc(window, message, wparam, lparam);
      if (message == WM_INITDIALOG) PostMessageW(window, WM_APP, 0, 0);
      return handled;
    };
    require(DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), nullptr,
        modal, reinterpret_cast<LPARAM>(&state)) == IDOK, "modal notes did not collapse or open at the first line");
  }
  for (const bool korean : {false, true}) {
    Dialog state{release, korean};
    state.download = fake_download; state.launch = fake_launch;
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), nullptr,
        update_proc, reinterpret_cast<LPARAM>(&state));
    if (!window) throw std::runtime_error("update resource did not load");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{window};
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_TOGGLE_NOTES, 0);
    require(SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_GETFIRSTVISIBLELINE, 0, 0) == 0,
        "release notes did not open at the first line");
    require(text(window, IDC_UPDATE_NOTES).find(L"\r\n") != std::wstring::npos, "notes line breaks lost");
    require(text(window, IDC_UPDATE_NOTES).find(L"\ud55c\uae00") != std::wstring::npos, "Korean notes lost");
    const auto rendered = text(window, IDC_UPDATE_NOTES);
    require(rendered.find(L"Excluded") == rendered.npos && rendered.find(L"Hidden") == rendered.npos &&
        rendered.find(L"##") == rendered.npos && rendered.find(L"**") == rendered.npos, "selected Markdown text not rendered");
    const auto bold_at = rendered.find(L"Bold");
    require(bold_at != rendered.npos, "bold text missing");
    FINDTEXTEXW find{};
    find.chrg = {0, -1}; find.lpstrText = L"Bold";
    require(SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&find)) >= 0, "bold range not found");
    SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&find.chrgText));
    CHARFORMAT2W character{sizeof(character)};
    SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&character));
    require((character.dwEffects & CFE_BOLD) != 0, "bold style missing in real Rich Edit");
    SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_SETSEL, 0, 0);
    require(text(window, IDC_UPDATE_ACTION) == (korean ? L"업데이트" : L"Update"), "update translation");
    RECT client{}; GetClientRect(window, &client);
    for (const int id : {IDC_UPDATE_NOTES, IDC_UPDATE_STATUS, IDC_UPDATE_PROGRESS, IDC_UPDATE_ACTION, IDCANCEL}) {
      RECT bounds{}; GetWindowRect(GetDlgItem(window, id), &bounds);
      MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&bounds), 2);
      require(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom,
          "update control clipped outside client area");
    }
    const int before = launches;
    const int downloads_before = downloads.load();
    cancelled_uac = true;
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    finish_worker(window, state);
    require(state.downloaded.status == DownloadStatus::ready, "UI download not ready");
    require(launches == before + 1 && !state.result.installer_started && state.downloaded.file,
        "verified download did not launch or UAC cancellation lost the file");
    cancelled_uac = false;
    CheckDlgButton(window, IDC_UPDATE_DISABLE, BST_CHECKED);
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    require(launches == before + 2 && downloads == downloads_before + 1 && state.result.installer_started && state.result.disable_notifications,
        "verified install/notification choice not propagated");
  }
  {
    auto empty = release; empty.release_notes.clear(); empty.sha256.clear();
    Dialog state{empty, false};
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), nullptr,
        update_proc, reinterpret_cast<LPARAM>(&state));
    if (!window) throw std::runtime_error("empty notes dialog creation");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{window};
    require((GetWindowLongPtrW(GetDlgItem(window, IDC_UPDATE_NOTES), GWL_STYLE) & WS_VISIBLE) == 0 &&
        (GetWindowLongPtrW(GetDlgItem(window, IDC_UPDATE_TOGGLE_NOTES), GWL_STYLE) & WS_VISIBLE) == 0 &&
        !IsWindowEnabled(GetDlgItem(window, IDC_UPDATE_ACTION)), "empty notes/unverifiable asset fallback");
    RECT client{}; GetClientRect(window, &client);
    RECT button{}; GetWindowRect(GetDlgItem(window, IDCANCEL), &button);
    MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&button), 2);
    require(button.top > 0 && button.bottom <= client.bottom, "compact update dialog clipped");
  }
  {
    // Corrupt files cannot reach even the fake launcher. Closing during an
    // outstanding worker requests cancellation and defers state destruction.
    Dialog state{release, false};
    state.download = fake_download; state.launch = fake_launch;
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), nullptr,
        update_proc, reinterpret_cast<LPARAM>(&state));
    if (!window) throw std::runtime_error("second update dialog creation");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{window};
    cancelled_uac = true;
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0); finish_worker(window, state);
    cancelled_uac = false;
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
  std::cout << "Update dialog localization, layout, download, retry, cancellation and verified launch passed.\n";
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
