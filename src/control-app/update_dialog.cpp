#include "control-app/update_dialog.hpp"
#include "control-app/release_notes.hpp"
#include "control-app/resource.h"
#include "control-app/update_download.hpp"
#include "soundfork_version.hpp"

#include <algorithm>
#include <commctrl.h>
#include <cstring>
#include <memory>
#include <richedit.h>
#include <shellapi.h>
#include <thread>

namespace soundfork::control {
namespace {
constexpr UINT_PTR kDownloadTimer = 1;
enum class NoticePhase { available, downloading, cancelling, failed, launching, install_cancelled, install_failed, closed };

struct Dialog {
  Dialog(UpdateResult value, bool ko) : release(std::move(value)), korean(ko) {}
  const UpdateResult release;
  bool korean;
  bool window_owned = false;
  NoticePhase phase = NoticePhase::available;
  DownloadProgress progress;
  DownloadedUpdate downloaded;
  std::atomic<bool> done{false};
  std::thread worker;
  void (*download)(const UpdateResult&, DownloadProgress&, DownloadedUpdate&) noexcept = download_update;
  bool (*launch)(HWND, bool, const std::filesystem::path&) = nullptr;
  ~Dialog() {
    progress.cancel = true;
    if (worker.joinable()) worker.join();
  }
  const wchar_t* text(const wchar_t* ko, const wchar_t* en) const noexcept { return korean ? ko : en; }
};

Dialog* notice_state(HWND window) noexcept {
  return reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
}

void update_notice_controls(HWND window, const Dialog& state) {
  const auto version = state.text(L"새 버전: ", L"New version: ") + state.release.latest_tag +
      state.text(L"    현재: v", L"    Current: v") + SOUNDFORK_VERSION_WIDE;
  SetDlgItemTextW(window, IDC_UPDATE_VERSION, version.c_str());
  const bool has_notes = !selected_release_notes(state.release.release_notes).empty();
  SetDlgItemTextW(window, IDC_UPDATE_TOGGLE_NOTES, has_notes
      ? state.text(L"변경사항 보기", L"Show changes") : state.text(L"릴리스 정보", L"Release details"));

  const bool transferable = downloadable_update(state.release);
  const bool downloading = state.phase == NoticePhase::downloading || state.phase == NoticePhase::cancelling;
  const bool launching = state.phase == NoticePhase::launching;
  const bool retry_install = state.phase == NoticePhase::install_cancelled || state.phase == NoticePhase::install_failed;
  SetDlgItemTextW(window, IDC_UPDATE_ACTION, retry_install ? state.text(L"설치 다시 시도", L"Retry setup")
      : state.phase == NoticePhase::failed ? state.text(L"다시 시도", L"Retry") : state.text(L"업데이트", L"Update"));
  SetDlgItemTextW(window, IDCANCEL, downloading ? state.text(L"취소", L"Cancel") : state.text(L"나중에", L"Later"));
  SetDlgItemTextW(window, IDC_UPDATE_DISABLE, state.text(L"앞으로 자동 업데이트 알림 표시 안 함", L"Don't show automatic update notifications again"));
  EnableWindow(GetDlgItem(window, IDC_UPDATE_ACTION), transferable && !downloading && !launching);
  EnableWindow(GetDlgItem(window, IDC_UPDATE_TOGGLE_NOTES), !downloading && !launching);
  EnableWindow(GetDlgItem(window, IDCANCEL), state.phase != NoticePhase::cancelling && !launching);

  const wchar_t* status = nullptr;
  switch (state.phase) {
    case NoticePhase::available: break;
    case NoticePhase::downloading: status = state.text(L"다운로드 중…", L"Downloading…"); break;
    case NoticePhase::cancelling: status = state.text(L"취소 중…", L"Cancelling…"); break;
    case NoticePhase::failed: status = state.text(L"다운로드 또는 검증 실패", L"Download or verification failed"); break;
    case NoticePhase::launching: status = state.text(L"설치 마법사를 여는 중…", L"Opening the setup wizard…"); break;
    case NoticePhase::install_cancelled: status = state.text(L"설치가 취소되었습니다.", L"Installation cancelled."); break;
    case NoticePhase::install_failed: status = state.text(L"설치 마법사를 열지 못했습니다.", L"Could not open Setup."); break;
    case NoticePhase::closed: break;
  }
  SetDlgItemTextW(window, IDC_UPDATE_STATUS, status ? status : L"");
  ShowWindow(GetDlgItem(window, IDC_UPDATE_DISABLE), status ? SW_HIDE : SW_SHOW);
  ShowWindow(GetDlgItem(window, IDC_UPDATE_STATUS), status ? SW_SHOW : SW_HIDE);
  ShowWindow(GetDlgItem(window, IDC_UPDATE_PROGRESS), downloading ? SW_SHOW : SW_HIDE);
}

void finish_notice(HWND window, Dialog& state, bool started = false) {
  state.phase = NoticePhase::closed;
  LPARAM flags = IsDlgButtonChecked(window, IDC_UPDATE_DISABLE) == BST_CHECKED ? disable_update_notifications : 0;
  if (started) flags |= update_installer_started;
  PostMessageW(GetParent(window), kUpdateNoticeClosed, reinterpret_cast<WPARAM>(window), flags);
}

void launch_verified_installer(HWND window, Dialog& state) {
  if (!verify_download(state.release, state.downloaded)) {
    state.downloaded.reset();
    state.phase = NoticePhase::failed;
    update_notice_controls(window, state);
    return;
  }
  state.phase = NoticePhase::launching;
  update_notice_controls(window, state);
  const bool launched = state.launch && state.launch(window, state.korean, state.downloaded.path);
  const auto error = GetLastError();
  if (launched) {
    state.downloaded.retain();
    finish_notice(window, state, true);
  } else {
    state.phase = error == ERROR_CANCELLED ? NoticePhase::install_cancelled : NoticePhase::install_failed;
    update_notice_controls(window, state);
  }
}

bool render_notes(HWND window, const UpdateResult& release) {
  const auto rtf = release_notes_rtf(release.release_notes);
  struct Stream { const std::string& text; std::size_t offset = 0; } input{rtf};
  EDITSTREAM stream{};
  stream.dwCookie = reinterpret_cast<DWORD_PTR>(&input);
  stream.pfnCallback = +[](DWORD_PTR cookie, LPBYTE buffer, LONG count, LONG* written) -> DWORD {
    auto& source = *reinterpret_cast<Stream*>(cookie);
    if (count < 0) return ERROR_INVALID_PARAMETER;
    const auto size = (std::min)(static_cast<std::size_t>(count), source.text.size() - source.offset);
    if (size) std::memcpy(buffer, source.text.data() + source.offset, size);
    source.offset += size;
    *written = static_cast<LONG>(size);
    return 0;
  };
  const HWND notes = GetDlgItem(window, IDC_UPDATE_NOTES);
  SendMessageW(notes, EM_EXLIMITTEXT, 0, 2U * 1024U * 1024U);
  SendMessageW(notes, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW));
  SendMessageW(notes, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream));
  SendMessageW(notes, EM_SETSEL, 0, 0);
  POINT top{};
  SendMessageW(notes, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&top));
  return stream.dwError == 0;
}

INT_PTR CALLBACK notes_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
  try {
    if (message == WM_INITDIALOG) {
      const auto& state = *reinterpret_cast<const Dialog*>(lparam);
      SetWindowLongPtrW(window, DWLP_USER, lparam);
      SetWindowTextW(window, state.text(L"변경사항", L"Release notes"));
      SetDlgItemTextW(window, IDC_UPDATE_RELEASE, state.text(L"릴리스 페이지", L"Release page"));
      SetDlgItemTextW(window, IDCANCEL, state.text(L"닫기", L"Close"));
      SetFocus(GetDlgItem(window, IDCANCEL));
      if (!render_notes(window, state.release)) EndDialog(window, -1);
      if (selected_release_notes(state.release.release_notes).empty()) {
        SetDlgItemTextW(window, IDC_UPDATE_NOTES, state.text(L"게시된 변경사항이 없습니다.", L"No release notes were published."));
      }
      return FALSE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
      EndDialog(window, IDCANCEL);
      return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDC_UPDATE_RELEASE) {
      const auto* state = notice_state(window);
      const auto url = L"https://github.com/kjh2623jh/SoundFork/releases/tag/" + state->release.latest_tag;
      if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        MessageBoxW(window, state->text(L"릴리스 페이지를 열지 못했습니다.", L"Could not open the release page."), L"SoundFork", MB_ICONERROR);
      }
      return TRUE;
    }
    return FALSE;
  } catch (...) {
    EndDialog(window, -1);
    return TRUE;
  }
}

void show_notes(HWND notice, const Dialog& state) {
  const HWND parent = GetParent(notice);
  const HMODULE rich_edit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  const auto result = rich_edit ? DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE_NOTES),
      parent, notes_proc, reinterpret_cast<LPARAM>(&state)) : -1;
  if (rich_edit) FreeLibrary(rich_edit);
  if (result == -1) {
    MessageBoxW(parent, state.text(L"변경사항을 열지 못했습니다.", L"Could not show the release notes."), L"SoundFork", MB_ICONERROR);
  }
}

INT_PTR CALLBACK update_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
  auto* state = notice_state(window);
  try {
    if (message == WM_INITDIALOG) {
      state = reinterpret_cast<Dialog*>(lparam);
      SetWindowLongPtrW(window, DWLP_USER, lparam);
      update_notice_controls(window, *state);
      SendDlgItemMessageW(window, IDC_UPDATE_PROGRESS, PBM_SETRANGE32, 0, 100);
      return FALSE;
    }
    if (!state) return FALSE;
    if (message == WM_NCDESTROY) {
      SetWindowLongPtrW(window, DWLP_USER, 0);
      if (state->window_owned) delete state;
      return FALSE;
    }
    if (message == WM_TIMER && wparam == kDownloadTimer && state->worker.joinable()) {
      const auto total = state->release.installer_size;
      const auto received = state->progress.received.load();
      SendDlgItemMessageW(window, IDC_UPDATE_PROGRESS, PBM_SETPOS, total ? received * 100 / total : 0, 0);
      if (!state->done.load(std::memory_order_acquire)) return TRUE;
      KillTimer(window, kDownloadTimer);
      state->worker.join();
      if (state->phase == NoticePhase::cancelling) {
        finish_notice(window, *state);
      } else if (state->downloaded.status == DownloadStatus::ready) {
        launch_verified_installer(window, *state);
      } else {
        state->phase = NoticePhase::failed;
        update_notice_controls(window, *state);
      }
      return TRUE;
    }
    if (message != WM_COMMAND || state->phase == NoticePhase::launching || state->phase == NoticePhase::closed) return FALSE;
    const int command = LOWORD(wparam);
    if (command == IDC_UPDATE_TOGGLE_NOTES && !state->worker.joinable()) {
      show_notes(window, *state);
      return TRUE;
    }
    if (command == IDCANCEL) {
      if (state->worker.joinable()) {
        state->phase = NoticePhase::cancelling;
        state->progress.cancel = true;
        update_notice_controls(window, *state);
      } else {
        finish_notice(window, *state);
      }
      return TRUE;
    }
    if (command != IDC_UPDATE_ACTION || !IsWindowEnabled(GetDlgItem(window, IDC_UPDATE_ACTION))) return FALSE;
    if (state->downloaded.status == DownloadStatus::ready) {
      launch_verified_installer(window, *state);
      return TRUE;
    }
    if (!SetTimer(window, kDownloadTimer, 100, nullptr)) {
      state->phase = NoticePhase::failed;
      update_notice_controls(window, *state);
      return TRUE;
    }
    state->progress.cancel = false;
    state->progress.received = 0;
    state->done = false;
    SendDlgItemMessageW(window, IDC_UPDATE_PROGRESS, PBM_SETPOS, 0, 0);
    try {
      state->worker = std::thread([state] {
        state->download(state->release, state->progress, state->downloaded);
        state->done.store(true, std::memory_order_release);
      });
      state->phase = NoticePhase::downloading;
    } catch (...) {
      KillTimer(window, kDownloadTimer);
      state->phase = NoticePhase::failed;
    }
    update_notice_controls(window, *state);
    return TRUE;
  } catch (...) {
    if (state) {
      state->progress.cancel = true;
      if (state->worker.joinable()) state->phase = NoticePhase::cancelling;
      else finish_notice(window, *state);
    }
    return TRUE;
  }
}
}  // namespace

HWND create_update_notice(HWND parent, bool korean, UpdateResult release) {
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
  if (!InitCommonControlsEx(&controls)) return nullptr;
  auto state = std::make_unique<Dialog>(std::move(release), korean);
  state->launch = +[](HWND window, bool ko, const std::filesystem::path& path) {
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.hwnd = GetParent(window);
    launch.lpVerb = L"runas";
    launch.lpFile = path.c_str();
    launch.lpParameters = ko ? L"/LANG=korean" : L"/LANG=english";
    launch.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&launch) != FALSE;
  };
  const HWND window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_UPDATE), parent,
      update_proc, reinterpret_cast<LPARAM>(state.get()));
  if (window) state.release()->window_owned = true;
  return window;
}

void translate_update_notice(HWND notice, bool korean) {
  if (auto* state = notice_state(notice)) {
    state->korean = korean;
    update_notice_controls(notice, *state);
  }
}
}  // namespace soundfork::control
