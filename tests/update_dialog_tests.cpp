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
  const HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW,
      0, 0, 600, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!parent) throw std::runtime_error("test parent creation");
  struct Window { HWND value; ~Window() { DestroyWindow(value); } } parent_owner{parent};
  {
    Dialog state{release, true};
    const HMODULE rich = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!rich) throw std::runtime_error("Rich Edit DLL unavailable");
    struct Library { HMODULE value; ~Library() { FreeLibrary(value); } } rich_owner{rich};
    const auto modal = +[](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> INT_PTR {
      if (message == WM_APP) {
        ValidateRect(window, nullptr);
        CHARRANGE selected{};
        SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selected));
        const bool at_top = SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_GETFIRSTVISIBLELINE, 0, 0) == 0;
        const bool no_selection = selected.cpMin == 0 && selected.cpMax == 0;
        const auto rendered = text(window, IDC_UPDATE_NOTES);
        FINDTEXTEXW find{};
        find.chrg = {0, -1}; find.lpstrText = L"Bold";
        const auto found = SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&find));
        SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&find.chrgText));
        CHARFORMAT2W character{sizeof(character)};
        SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&character));
        const bool notes_ok = found >= 0 && (character.dwEffects & CFE_BOLD) && rendered.find(L"\ud55c\uae00") != rendered.npos &&
            rendered.find(L"Excluded") == rendered.npos && rendered.find(L"Hidden") == rendered.npos;
        EndDialog(window, at_top && no_selection && notes_ok && GetDlgItem(window, IDC_UPDATE_RELEASE) ? IDOK : IDCANCEL);
        return TRUE;
      }
      const auto handled = notes_proc(window, message, wparam, lparam);
      if (message == WM_INITDIALOG) PostMessageW(window, WM_APP, 0, 0);
      return handled;
    };
    require(DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE_NOTES), parent,
        modal, reinterpret_cast<LPARAM>(&state)) == IDOK, "notes text, styling, scroll or release link");
  }
  for (const bool korean : {false, true}) {
    Dialog state{release, korean};
    state.download = fake_download; state.launch = fake_launch;
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), parent,
        update_proc, reinterpret_cast<LPARAM>(&state));
    if (!window) throw std::runtime_error("update resource did not load");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{window};
    require(GetParent(window) == parent && !GetDlgItem(window, IDC_UPDATE_RELEASE), "notice is not embedded or exposes release link");
    require(text(window, IDC_UPDATE_ACTION) == (korean ? L"업데이트" : L"Update"), "update translation");
    RECT client{}; GetClientRect(window, &client);
    for (const int id : {IDC_UPDATE_TOGGLE_NOTES, IDC_UPDATE_STATUS, IDC_UPDATE_PROGRESS, IDC_UPDATE_ACTION, IDCANCEL}) {
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
    require(launches == before + 1 && state.phase == NoticePhase::install_cancelled && state.downloaded.file,
        "verified download did not launch or UAC cancellation lost the file");
    cancelled_uac = false;
    CheckDlgButton(window, IDC_UPDATE_DISABLE, BST_CHECKED);
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    MSG notification{};
    const bool posted = PeekMessageW(&notification, parent, kUpdateNoticeClosed, kUpdateNoticeClosed, PM_REMOVE) != FALSE;
    require(launches == before + 2 && downloads == downloads_before + 1 && posted &&
        notification.lParam == (disable_update_notifications | update_installer_started),
        "verified install/notification choice not propagated");
  }
  {
    auto empty = release; empty.release_notes.clear(); empty.sha256.clear();
    Dialog state{empty, false};
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), parent,
        update_proc, reinterpret_cast<LPARAM>(&state));
    if (!window) throw std::runtime_error("empty notes dialog creation");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{window};
    require(text(window, IDC_UPDATE_TOGGLE_NOTES) == L"Release details" && !IsWindowEnabled(GetDlgItem(window, IDC_UPDATE_ACTION)), "missing notes or installer fallback");
    RECT client{}; GetClientRect(window, &client);
    RECT button{}; GetWindowRect(GetDlgItem(window, IDCANCEL), &button);
    MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&button), 2);
    require(button.top > 0 && button.bottom <= client.bottom, "compact update dialog clipped");
  }
  {
    auto corrupt = release;
    corrupt.sha256[0] = '0';
    Dialog state{corrupt, false};
    state.download = fake_download;
    state.launch = fake_launch;
    HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), parent,
        update_proc, reinterpret_cast<LPARAM>(&state));
    if (!window) throw std::runtime_error("failure test notice creation");
    Window owner{window};
    const int before = launches;
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    finish_worker(window, state);
    require(launches == before && !state.downloaded.file && state.phase == NoticePhase::failed &&
        IsWindowEnabled(GetDlgItem(window, IDC_UPDATE_ACTION)), "corrupt file launched or retry unavailable");
    state.download = +[](const UpdateResult&, DownloadProgress& progress, DownloadedUpdate& output) noexcept {
      while (!progress.cancel.load()) Sleep(1);
      output.status = DownloadStatus::cancelled;
    };
    SendMessageW(window, WM_COMMAND, IDC_UPDATE_ACTION, 0);
    SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    require(state.phase == NoticePhase::cancelling && state.progress.cancel && !IsWindowEnabled(GetDlgItem(window, IDCANCEL)), "close did not defer cancellation");
    finish_worker(window, state);
  }
  std::cout << "Update dialog localization, layout, download, retry, cancellation and verified launch passed.\n";
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
