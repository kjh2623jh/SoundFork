#include "control-app/update_dialog.hpp"
#include "control-app/update_download.hpp"
#include "control-app/resource.h"
#include "soundfork_version.hpp"

#include <commctrl.h>
#include <richedit.h>
#include <cstring>
#include <algorithm>
#include "control-app/release_notes.hpp"
#include <shellapi.h>
#include <thread>
#include <stdexcept>

namespace soundfork::control {
namespace {
constexpr UINT_PTR kDownloadTimer = 1;
struct Dialog {
  Dialog(const UpdateResult& value, bool ko) : release(value), korean(ko) {}
  HMODULE rich_edit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  const UpdateResult& release;
  bool korean;
  UpdateDialogResult result;
  DownloadProgress progress;
  DownloadedUpdate downloaded;
  std::atomic<bool> done{false};
  std::thread worker;
  bool closing = false;
  bool launching = false;
  void (*download)(const UpdateResult&, DownloadProgress&, DownloadedUpdate&) noexcept = download_update;
  bool (*launch)(HWND, bool, const std::filesystem::path&) = nullptr;
  ~Dialog() { progress.cancel = true; if (worker.joinable()) worker.join(); if (rich_edit) FreeLibrary(rich_edit); }
  const wchar_t* text(const wchar_t* ko, const wchar_t* en) const noexcept { return korean ? ko : en; }
};

void close_dialog(HWND window, Dialog& state) {
  if (state.launching) return;
  if (state.worker.joinable()) {
    state.closing = true;
    state.progress.cancel = true;
    EnableWindow(GetDlgItem(window, IDCANCEL), FALSE);
    SetDlgItemTextW(window, IDC_UPDATE_STATUS, state.text(L"다운로드를 취소하고 있습니다…", L"Cancelling download…"));
    return;
  }
  state.result.disable_notifications = IsDlgButtonChecked(window, IDC_UPDATE_DISABLE) == BST_CHECKED;
  EndDialog(window, IDOK);
}

void launch_verified_installer(HWND window, Dialog& state) {
  if (!verify_download(state.release, state.downloaded)) {
    state.downloaded.reset();
    SetDlgItemTextW(window, IDC_UPDATE_STATUS, state.text(L"파일 확인에 실패했습니다. 다시 다운로드하세요.", L"File verification failed. Download again."));
    SetDlgItemTextW(window, IDC_UPDATE_ACTION, state.text(L"다시 시도", L"Retry"));
    return;
  }
  state.launching = true;
  SetDlgItemTextW(window, IDC_UPDATE_STATUS, state.text(L"설치 마법사를 여는 중…", L"Opening the setup wizard…"));
  const bool launched = state.launch && state.launch(window, state.korean, state.downloaded.path);
  const auto error = GetLastError();
  state.launching = false;
  if (!launched) {
    SetDlgItemTextW(window, IDC_UPDATE_STATUS, error == ERROR_CANCELLED
        ? state.text(L"설치가 취소되었습니다. 다시 시도할 수 있습니다.", L"Installation was cancelled. You can retry.")
        : state.text(L"설치 프로그램을 실행하지 못했습니다. 다시 시도하세요.", L"Could not start the installer. Please retry."));
    SetDlgItemTextW(window, IDC_UPDATE_ACTION, state.text(L"설치 다시 시도", L"Retry installation"));
    return;
  }
  state.downloaded.retain();
  state.result.installer_started = true;
  close_dialog(window, state);
}

INT_PTR update_proc_impl(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<Dialog*>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, lparam);
    const std::wstring title = state->text(L"새 버전: ", L"New version: ") + state->release.latest_tag +
        state->text(L"  ·  현재: v", L"  ·  Current: v") + SOUNDFORK_VERSION_WIDE;
    SetDlgItemTextW(window, IDC_UPDATE_VERSION, title.c_str());
    SetWindowTextW(window, state->text(L"SoundFork 업데이트", L"SoundFork update"));
    SetDlgItemTextW(window, IDC_UPDATE_NOTES_LABEL, state->text(L"업데이트 내용", L"Release notes"));
    const auto notes = selected_release_notes(state->release.release_notes);
    const auto rtf = release_notes_rtf(state->release.release_notes);
    struct Stream { const std::string& text; std::size_t offset = 0; } input{rtf};
    EDITSTREAM stream{};
    stream.dwCookie = reinterpret_cast<DWORD_PTR>(&input);
    stream.pfnCallback = +[](DWORD_PTR cookie, LPBYTE buffer, LONG count, LONG* written) -> DWORD {
      auto& source = *reinterpret_cast<Stream*>(cookie);
      if (count < 0) return ERROR_INVALID_PARAMETER;
      const auto size = (std::min)(static_cast<std::size_t>(count), source.text.size() - source.offset);
      if (size) std::memcpy(buffer, source.text.data() + source.offset, size);
      source.offset += size; *written = static_cast<LONG>(size); return 0;
    };
    SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_EXLIMITTEXT, 0, 2U * 1024U * 1024U);
    SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW));
    SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream));
    if (stream.dwError) throw std::runtime_error("Release notes rendering failed");
    ShowWindow(GetDlgItem(window, IDC_UPDATE_NOTES_LABEL), notes.empty() ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(window, IDC_UPDATE_NOTES), notes.empty() ? SW_HIDE : SW_SHOW);
    if (notes.empty()) {
      RECT removed{0, 0, 0, 150}; MapDialogRect(window, &removed);
      for (const int id : {IDC_UPDATE_STATUS, IDC_UPDATE_PROGRESS, IDC_UPDATE_DISABLE,
                          IDC_UPDATE_RELEASE, IDC_UPDATE_ACTION, IDCANCEL}) {
        HWND child = GetDlgItem(window, id);
        RECT bounds{}; GetWindowRect(child, &bounds);
        MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&bounds), 2);
        SetWindowPos(child, nullptr, bounds.left, bounds.top - removed.bottom, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      RECT bounds{}; GetWindowRect(window, &bounds);
      SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left,
          bounds.bottom - bounds.top - removed.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetDlgItemTextW(window, IDC_UPDATE_ACTION, state->text(L"다운로드 및 설치", L"Download and install"));
    SetDlgItemTextW(window, IDC_UPDATE_RELEASE, state->text(L"릴리스 페이지", L"Release page"));
    SetDlgItemTextW(window, IDCANCEL, state->text(L"나중에", L"Later"));
    SetDlgItemTextW(window, IDC_UPDATE_DISABLE, state->text(L"앞으로 자동 업데이트 알림 표시 안 함", L"Don't show automatic update notifications again"));
    const bool available = downloadable_update(state->release);
    EnableWindow(GetDlgItem(window, IDC_UPDATE_ACTION), available);
    SetDlgItemTextW(window, IDC_UPDATE_STATUS, available
        ? state->text(L"파일을 확인한 뒤 설치 마법사를 엽니다.", L"Setup opens after the download is verified.")
        : state->text(L"검증 가능한 설치 파일이 없습니다. 릴리스 페이지를 확인하세요.", L"No verifiable installer is available. See the release page."));
    SendDlgItemMessageW(window, IDC_UPDATE_PROGRESS, PBM_SETRANGE32, 0, 100);
    if (!SetTimer(window, kDownloadTimer, 100, nullptr)) EndDialog(window, IDCANCEL);
    // Win32 otherwise focuses the first tab stop after WM_INITDIALOG and
    // selects all text in Rich Edit, which can scroll to the end.
    SetFocus(GetDlgItem(window, IDCANCEL));
    SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_SETSEL, 0, 0);
    POINT top{};
    SendDlgItemMessageW(window, IDC_UPDATE_NOTES, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&top));
    return FALSE;
  }
  if (!state) return FALSE;
  if (message == WM_TIMER && wparam == kDownloadTimer && state->worker.joinable()) {
    const auto received = state->progress.received.load();
    const auto total = state->release.installer_size;
    SendDlgItemMessageW(window, IDC_UPDATE_PROGRESS, PBM_SETPOS,
        total ? static_cast<WPARAM>(received * 100 / total) : 0, 0);
    if (!state->done.load(std::memory_order_acquire)) return TRUE;
    state->worker.join();
    if (state->closing) { close_dialog(window, *state); return TRUE; }
    const bool ready = state->downloaded.status == DownloadStatus::ready;
    EnableWindow(GetDlgItem(window, IDC_UPDATE_ACTION), TRUE);
    EnableWindow(GetDlgItem(window, IDC_UPDATE_RELEASE), TRUE);
    SetDlgItemTextW(window, IDCANCEL, state->text(L"닫기", L"Close"));
    if (ready) {
      launch_verified_installer(window, *state);
    } else {
      SetDlgItemTextW(window, IDC_UPDATE_STATUS, state->text(L"다운로드 또는 파일 확인에 실패했습니다. 다시 시도할 수 있습니다.", L"Download or verification failed. You can retry."));
      SetDlgItemTextW(window, IDC_UPDATE_ACTION, state->text(L"다시 시도", L"Retry"));
    }
    return TRUE;
  }
  if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
    close_dialog(window, *state); return TRUE;
  }
  if (message != WM_COMMAND || state->launching) return FALSE;
  if (LOWORD(wparam) == IDC_UPDATE_RELEASE) {
    const std::wstring url = L"https://github.com/kjh2623jh/SoundFork/releases/tag/" + state->release.latest_tag;
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
      MessageBoxW(window, state->text(L"릴리스 페이지를 열지 못했습니다.", L"Could not open the release page."), L"SoundFork", MB_ICONERROR);
    return TRUE;
  }
  if (LOWORD(wparam) != IDC_UPDATE_ACTION || state->worker.joinable()) return FALSE;
  if (!IsWindowEnabled(GetDlgItem(window, IDC_UPDATE_ACTION))) return TRUE;
  if (state->downloaded.status == DownloadStatus::ready) {
    launch_verified_installer(window, *state);
    return TRUE;
  }
  state->progress.cancel = false; state->progress.received = 0; state->done = false;
  try {
    state->worker = std::thread([state]() {
      state->download(state->release, state->progress, state->downloaded);
      state->done.store(true, std::memory_order_release);
    });
    EnableWindow(GetDlgItem(window, IDC_UPDATE_ACTION), FALSE);
    EnableWindow(GetDlgItem(window, IDC_UPDATE_RELEASE), FALSE);
    SetDlgItemTextW(window, IDCANCEL, state->text(L"취소", L"Cancel"));
    SetDlgItemTextW(window, IDC_UPDATE_STATUS, state->text(L"다운로드 중…", L"Downloading…"));
  } catch (...) {
    SetDlgItemTextW(window, IDC_UPDATE_STATUS, state->text(L"다운로드를 시작하지 못했습니다.", L"Could not start the download."));
  }
  return TRUE;
}

INT_PTR CALLBACK update_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
  try { return update_proc_impl(window, message, wparam, lparam); }
  catch (...) {
    auto* state = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    // Never let a C++ exception cross a Windows dialog callback, or destroy
    // worker-owned state before cancellation has completed.
    if (state) close_dialog(window, *state);
    else EndDialog(window, IDCANCEL);
    return TRUE;
  }
}
}  // namespace

UpdateDialogResult show_update_dialog(HWND parent, bool korean, const UpdateResult& release) {
  Dialog state{release, korean};
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
  if (!InitCommonControlsEx(&controls)) {
    MessageBoxW(parent, state.text(L"업데이트 창을 준비하지 못했습니다.", L"Could not prepare the update window."), L"SoundFork", MB_ICONERROR);
    return {};
  }
  state.launch = +[](HWND window, bool ko, const std::filesystem::path& path) {
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.hwnd = window; launch.lpVerb = L"runas"; launch.lpFile = path.c_str();
    launch.lpParameters = ko ? L"/LANG=korean" : L"/LANG=english";
    launch.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&launch) != FALSE;
  };
  if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), parent,
      update_proc, reinterpret_cast<LPARAM>(&state)) == -1) {
    MessageBoxW(parent, state.text(L"업데이트 창을 열지 못했습니다.", L"Could not open the update window."), L"SoundFork", MB_ICONERROR);
  }
  return state.result;
}
}  // namespace soundfork::control
