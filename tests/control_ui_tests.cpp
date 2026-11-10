// Exercise production widgets/commands with a private HKCU registry override.
// Never acquires the installed control mutex, adds a tray icon, starts audio,
// opens a browser, installs plugins, contacts GitHub or changes live settings.
#define wWinMain soundfork_control_unused_entry
#include "../src/control-app/control_main.cpp"
#undef wWinMain

#include <iostream>
#include <stdexcept>

namespace {
void require_ui(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct PrivatePreferences {
  HKEY key = nullptr;
  std::wstring path = L"Software\\SoundForkUiTest-" + std::to_wstring(GetCurrentProcessId());
  PrivatePreferences() {
    require_ui(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS, "private preferences creation failed");
    if (RegOverridePredefKey(HKEY_CURRENT_USER, key) != ERROR_SUCCESS) {
      RegCloseKey(key); key = nullptr;
      RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
      throw std::runtime_error("private preferences override failed");
    }
  }
  ~PrivatePreferences() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (key) RegCloseKey(key);
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  }
};

LRESULT CALLBACK test_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CREATE) return 0;  // Construct explicitly; no production tray/timer.
  if (message == WM_COMMAND) {
    const auto id = LOWORD(wparam);
    const bool navigation = id >= kNavigate && id < kNavigate + kPageCount;
    const bool preference = id == kChannels || id == kDisplayLanguage || id == kAutomaticUpdateNotifications;
    if (!navigation && !preference && id != kShowDiagnostics) return 0;
  }
  return window_proc(window, message, wparam, lparam);
}
std::wstring caption(HWND control) {
  std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
  GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
  text.resize(std::wcslen(text.c_str()));
  return text;
}
void check_pages(AppState& state) {
  const HWND window = state.window;
  for (const auto language : {UiLanguage::korean, UiLanguage::english}) {
    apply_language(&state, language, false);
    const bool ko = language == UiLanguage::korean;
    const wchar_t* expected[] = {ko?L"재생":L"Playback", ko?L"방송":L"Broadcast",
        ko?L"버퍼·채널":L"Buffer/ch.", L"OBS", ko?L"설정":L"Settings",
        ko?L"연결 정보":L"Connection", ko?L"프로그램 정보":L"About"};
    for (int page = 0; page < kPageCount; ++page) {
      SendMessageW(window, WM_COMMAND, kNavigate + page, 0);
      require_ui(state.panel.page == page, "navigation command not connected");
      require_ui(GetMenuItemCount(GetMenu(window)) == kPageCount, "navigation count wrong");
      std::array<wchar_t, 64> text{};
      require_ui(GetMenuStringW(GetMenu(window), static_cast<UINT>(page), text.data(),
          static_cast<int>(text.size()), MF_BYPOSITION) > 0, "navigation label missing");
      require_ui(std::wstring_view(text.data()) == expected[page], "Unicode navigation label wrong");
      require_ui(GetSubMenu(GetMenu(window), page) == nullptr, "nested menu remains");
      require_ui(GetMenuDefaultItem(GetMenu(window), TRUE, 0) == static_cast<UINT>(page), "current page not highlighted");
      RECT client{};
      GetClientRect(window, &client);
      for (int other = 0; other < kPageCount; ++other) {
        for (const auto control : state.panel.pages[other]) {
          if (!control) throw std::runtime_error("control creation failed");
          const bool visible = (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
          require_ui(visible == (other == page), "controls leak across pages");
          if (!visible) continue;
          RECT rect{};
          GetWindowRect(control, &rect);
          MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
          require_ui(rect.left >= 12 && rect.top >= 12 && rect.right - rect.left >= 40 &&
              rect.bottom - rect.top >= 16 && rect.right <= client.right &&
              rect.bottom <= client.bottom - 26, "page clipped or overlaps footer at minimum size");
        }
      }
      // Worker completion/menu rebuilding and language changes must not reset navigation.
      rebuild_menu(&state);
      require_ui(state.panel.page == page, "menu refresh reset page");
    }
    // Exercise the reported direction-sensitive path as well: every transition
    // below moves from a later page to an earlier page.
    for (int page = kPageCount - 1; page > 0; --page) {
      SendMessageW(window, WM_COMMAND, kNavigate + page, 0);
      SendMessageW(window, WM_COMMAND, kNavigate + page - 1, 0);
      UpdateWindow(window);
      for (int other = 0; other < kPageCount; ++other)
        for (const auto control : state.panel.pages[other])
          require_ui(IsWindowVisible(control) == (other == page - 1),
                     "backward navigation left an old page visible");
    }
    require_ui(caption(state.startup_checkbox) == startup_strings(language).menu, "startup translation wrong");
    require_ui(caption(state.panel.logs_button) == soundfork::control::diagnostic_strings(ko).logs + std::wstring(L"…"),
        "logs action translation wrong");
    require_ui(SendMessageW(state.panel.information, LVM_GETITEMCOUNT, 0, 0) == 6, "connection properties missing");
    require_ui(GetDlgItem(window, kCheckForUpdates) == state.panel.update_button, "update command missing");
    require_ui(GetDlgItem(window, kReportBug) == state.panel.bug_button, "report command missing");
    require_ui(GetDlgItem(window, kOpenProject) == state.panel.project_button, "project command missing");
  }
}
void check_channels(AppState& state) {
  for (const auto language : {UiLanguage::korean, UiLanguage::english}) {
    apply_language(&state, language, false);
    require_ui(SendMessageW(state.panel.channels, CB_GETCOUNT, 0, 0) == 33, "pair choices missing");
    for (const int index : {0, 1, 31}) {
      SendMessageW(state.panel.channels, CB_SETCURSEL, index, 0);
      SendMessageW(state.window, WM_COMMAND, MAKEWPARAM(kChannels, CBN_SELCHANGE), 0);
      const auto saved = soundfork::capture::load_channels();
      require_ui(saved.left == static_cast<DWORD>(index * 2) && saved.right == saved.left + 1,
          "pair command did not save selected channels");
    }
    save_channel_selection(&state, {63, 2});
    require_ui(SendMessageW(state.panel.channels, CB_GETCURSEL, 0, 0) == 32, "custom mapping not selected");
    for (bool accept : {false, true}) {
      ChannelDialog data;
      data.pair = {63, 2}; data.korean = language == UiLanguage::korean; data.font = state.font;
      const HWND dialog = CreateWindowExW(WS_EX_CONTROLPARENT, kChannelDialogClass, L"Channels",
          WS_OVERLAPPEDWINDOW, 0, 0, 412, 234, state.window, nullptr, GetModuleHandleW(nullptr), &data);
      if (!dialog || !data.left || !data.right) throw std::runtime_error("channel dialog missing");
      require_ui(SendMessageW(data.left, CB_GETCOUNT, 0, 0) == 64 &&
          SendMessageW(data.right, CB_GETCOUNT, 0, 0) == 64, "custom channels missing");
      require_ui(SendMessageW(data.left, CB_GETCURSEL, 0, 0) == 63 &&
          SendMessageW(data.right, CB_GETCURSEL, 0, 0) == 2, "custom channels initialized incorrectly");
      SendMessageW(data.left, CB_SETCURSEL, 0, 0);
      SendMessageW(data.right, CB_SETCURSEL, 0, 0);
      SendMessageW(dialog, WM_COMMAND, accept ? IDOK : IDCANCEL, 0);
      require_ui(!IsWindow(dialog) && data.accepted == accept, "dialog accept/cancel wrong");
      require_ui(accept ? data.pair.left == 0 && data.pair.right == 0
                        : data.pair.left == 63 && data.pair.right == 2, "dialog transaction wrong");
      require_ui(soundfork::capture::load_channels().left == 63, "dialog itself wrote preferences");
    }
  }
}
void check_obs(AppState& state) {
  state.obs_status = {};
  update_obs_controls(&state);
  require_ui(!IsWindowEnabled(state.panel.obs_install) && !IsWindowEnabled(state.panel.obs_remove),
      "missing plugin payload enabled actions");
  state.obs_status.bundled = state.obs_status.safe = true;
  update_obs_controls(&state);
  require_ui(IsWindowEnabled(state.panel.obs_install) && !IsWindowEnabled(state.panel.obs_remove),
      "fresh plugin actions wrong");
  state.obs_status.present = state.obs_status.complete = state.obs_status.update_required = true;
  state.obs_status.installed_version = L"1.0.1";
  for (const auto language : {UiLanguage::korean, UiLanguage::english}) {
    state.language = language;
    update_obs_controls(&state);
    const auto warning = language == UiLanguage::korean ? L"업데이트 필요" : L"update available";
    require_ui(caption(state.panel.obs_status).find(warning) != std::wstring::npos, "old plugin warning missing");
    require_ui(IsWindowEnabled(state.panel.obs_remove), "installed plugin removal disabled");
  }
  state.obs_operation_in_progress = true;
  update_obs_controls(&state);
  require_ui(!IsWindowEnabled(state.panel.obs_install) && !IsWindowEnabled(state.panel.obs_remove),
      "concurrent plugin actions enabled");
  state.obs_operation_in_progress = false;
  state.obs_status.safe = false;
  update_obs_controls(&state);
  require_ui(!IsWindowEnabled(state.panel.obs_install) && !IsWindowEnabled(state.panel.obs_remove),
      "unsafe plugin path enabled actions");
}
}  // namespace

int wmain() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
  struct ComEnd { ~ComEnd() { CoUninitialize(); } } com_end;
  try {
    PrivatePreferences preferences;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES};
    require_ui(InitCommonControlsEx(&common) != FALSE, "common controls failed");
    WNDCLASSEXW definition{sizeof(definition)};
    definition.lpfnWndProc = test_proc;
    definition.hInstance = GetModuleHandleW(nullptr);
    definition.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    definition.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    definition.lpszClassName = L"SoundForkControlUiTest";
    require_ui(RegisterClassExW(&definition) != 0, "test class failed");
    definition.lpfnWndProc = diagnostic_proc;
    definition.lpszClassName = kDiagnosticClass;
    require_ui(RegisterClassExW(&definition) != 0, "diagnostic class failed");
    definition.lpfnWndProc = channel_dialog;
    definition.lpszClassName = kChannelDialogClass;
    require_ui(RegisterClassExW(&definition) != 0, "channel dialog class failed");
    AppState state;
    state.language = UiLanguage::korean;
    state.automatic_updates_enabled = false;
    state.force_exit = true;
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"SoundForkControlUiTest", L"SoundFork control tests",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 540, 410, nullptr, nullptr,
        GetModuleHandleW(nullptr), &state);
    if (!window) throw std::runtime_error("test window failed");
    struct WindowEnd { HWND value; ~WindowEnd() { if (IsWindow(value)) DestroyWindow(value); } } window_end{window};
    require_ui(create_controls(&state), "production controls failed");
    rebuild_menu(&state);
    populate_backend_combo(&state);
    populate_endpoint_combo(&state);
    populate_relay_mode_combo(&state);
    update_status(&state);
    require_ui((GetWindowLongPtrW(state.status, GWL_STYLE) & WS_VSCROLL) == 0, "main status scrolls");
    SendMessageW(window, kAutomaticUpdateMessage, 0, 0);
    require_ui(!state.update_check_in_progress && !state.automatic_update_considered, "hidden control started network check");
    static std::atomic<int> update_calls{0};
    const auto fake_check = +[]() noexcept -> UpdateResult { ++update_calls; return {}; };
    begin_automatic_update_once(&state, fake_check);
    require_ui(!state.automatic_update_considered, "disabled update preference ignored");
    state.automatic_updates_enabled = true;
    begin_automatic_update_once(&state, fake_check);
    require_ui(!IsWindowEnabled(state.panel.update_button), "busy update button enabled");
    begin_automatic_update_once(&state, fake_check);
    require_ui(state.silent_update_request, "background request not silent");
    if (state.update_thread.joinable()) state.update_thread.join();
    require_ui(update_calls == 1, "automatic update did not run exactly once");
    SendMessageW(window, kUpdateResultMessage, 0, 0);
    require_ui(!state.update_check_in_progress && IsWindowEnabled(state.panel.update_button), "silent update did not finish");
    state.automatic_updates_enabled = false;
    MSG pending{};
    while (PeekMessageW(&pending, window, kUpdateResultMessage, kUpdateResultMessage, PM_REMOVE)) {}
    SendMessageW(window, WM_COMMAND, kAutomaticUpdateNotifications, 0);
    require_ui(state.automatic_updates_enabled && soundfork::control::automatic_updates_enabled(), "notification command not saved");
    SendMessageW(window, WM_COMMAND, kAutomaticUpdateNotifications, 0);
    require_ui(!state.automatic_updates_enabled && !soundfork::control::automatic_updates_enabled(), "notification command not disabled");
    for (const int index : {0, 1}) {
      SendMessageW(state.panel.language, CB_SETCURSEL, index, 0);
      SendMessageW(window, WM_COMMAND, MAKEWPARAM(kDisplayLanguage, CBN_SELCHANGE), 0);
      require_ui(state.language == (index == 0 ? UiLanguage::korean : UiLanguage::english), "language command not connected");
      require_ui(load_ui_language() == state.language, "language command not persisted");
    }
    const auto expected_startup = soundfork::control::RelaySettings{}.startup_preferred(executable_path())
        ? BST_CHECKED : BST_UNCHECKED;
    require_ui(GetDlgItem(window, kStartupRelay) == state.startup_checkbox, "startup checkbox missing");
    SendMessageW(state.startup_checkbox, BM_SETCHECK, expected_startup == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
    update_status(&state);
    require_ui(SendMessageW(state.startup_checkbox, BM_GETCHECK, 0, 0) == expected_startup, "startup preference not synchronized");
    check_pages(state);
    check_channels(state);
    check_obs(state);
    for (const auto language : {UiLanguage::korean, UiLanguage::english}) {
      apply_language(&state, language, false);
      const auto& text = soundfork::control::diagnostic_strings(language == UiLanguage::korean);
      HWND diagnostic = CreateWindowExW(WS_EX_CONTROLPARENT, kDiagnosticClass, text.title,
          WS_OVERLAPPEDWINDOW, 0, 0, 740, 480, window, nullptr, GetModuleHandleW(nullptr), &state);
      if (!diagnostic || !state.diagnostic_copy || !state.diagnostic_close)
        throw std::runtime_error("diagnostic controls missing");
      update_status(&state);
      require_ui(GetWindowTextLengthW(state.diagnostic_text) > 0, "diagnostics empty");
      require_ui(state.diagnostic_snapshot.starts_with(L"SoundFork v"), "diagnostics version missing");
      apply_language(&state, language == UiLanguage::korean ? UiLanguage::english : UiLanguage::korean, false);
      SendMessageW(diagnostic, WM_COMMAND, kCloseDiagnostics, 0);
      require_ui(!state.diagnostic_window && !state.diagnostic_text, "diagnostic handles not cleaned");
    }
    DestroyWindow(window);
    std::cout << "Production navigation, layout, isolated preferences, OBS action states and diagnostics passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
