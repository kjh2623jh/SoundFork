// Exercise the real native controls without using the installed GUI mutex,
// touching preferences, adding a tray icon or starting/stopping audio processes.
#define wWinMain soundfork_control_unused_entry
#include "../src/control-app/control_main.cpp"
#undef wWinMain

#include <iostream>
#include <array>
#include <stdexcept>

namespace {
void require_ui(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LRESULT CALLBACK preview_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CREATE) return 0;  // Initialize explicitly below, no production tray/timer.
  if (message == WM_COMMAND) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state != nullptr && (LOWORD(wparam) == kLanguageKorean || LOWORD(wparam) == kLanguageEnglish)) {
      apply_language(state, LOWORD(wparam) == kLanguageKorean ? UiLanguage::korean : UiLanguage::english, false);
      SetWindowTextW(window, L"SoundFork UI preview (read-only)");
      return 0;
    }
    if (LOWORD(wparam) != kShowDiagnostics) return 0;  // All mutating/online commands disabled in this test host.
  }
  return window_proc(window, message, wparam, lparam);
}
}  // namespace

int wmain(int argc, wchar_t**) {
  const bool interactive = argc == 1;  // CTest always passes --test.
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
  struct ComEnd { ~ComEnd() { CoUninitialize(); } } com_end;
  try {
    for(bool korean : {false,true}) {
      HMENU channels=capture_channel_menu(korean,{63,2});
      require_ui(channels!=nullptr && GetMenuItemCount(channels)==8,"stereo pair menu missing");
      const auto more=GetSubMenu(channels,5);
      require_ui(GetMenuItemCount(more)==28,"additional stereo pairs missing");
      const auto custom=GetSubMenu(channels,7);
      require_ui((GetMenuState(channels,7,MF_BYPOSITION)&MF_CHECKED)!=0,"custom mapping not indicated");
      const auto left=GetSubMenu(custom,1);
      const auto right=GetSubMenu(custom,2);
      require_ui(GetMenuItemCount(left)==4 && GetMenuItemCount(right)==4,"channel ranges missing");
      require_ui((GetMenuState(GetSubMenu(left,3),kCaptureLeftBase+63,MF_BYCOMMAND)&MF_CHECKED)!=0,"left channel check wrong");
      require_ui((GetMenuState(GetSubMenu(right,0),kCaptureRightBase+2,MF_BYCOMMAND)&MF_CHECKED)!=0,"right channel check wrong");
      DestroyMenu(channels);
      channels=capture_channel_menu(korean,{62,63});
      require_ui((GetMenuState(GetSubMenu(channels,5),kCapturePairBase+31,MF_BYCOMMAND)&MF_CHECKED)!=0,
                 "last stereo pair not indicated");
      require_ui((GetMenuState(channels,7,MF_BYPOSITION)&MF_CHECKED)==0,"stereo pair marked custom");
      DestroyMenu(channels);
    }
    WNDCLASSEXW definition{sizeof(definition)};
    definition.lpfnWndProc = preview_proc;
    definition.hInstance = GetModuleHandleW(nullptr);
    definition.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    definition.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    definition.lpszClassName = L"SoundForkReadOnlyControlTest";
    require_ui(RegisterClassExW(&definition) != 0, "preview class failed");
    definition.lpfnWndProc = diagnostic_proc;
    definition.lpszClassName = kDiagnosticClass;
    require_ui(RegisterClassExW(&definition) != 0, "diagnostic class failed");
    AppState state;
    // The preview must never contact GitHub or change notification preferences.
    state.automatic_updates_enabled = false;
    state.language = UiLanguage::korean;
    state.obs_status.present = state.obs_status.complete = true;
    state.obs_status.installed_version = L"1.0.1";
    state.obs_status.update_required = true;
    require_ui(obs_installed_text(&state).find(L"업데이트 필요") != std::wstring::npos, "old plugin Korean warning missing");
    state.language = UiLanguage::english;
    require_ui(obs_installed_text(&state).find(L"Update required") != std::wstring::npos, "old plugin English warning missing");
    state.language = UiLanguage::korean;
    state.obs_status = {};
    state.force_exit = true;  // Closing this test host never touches the installed relay.
    HWND window = CreateWindowExW(0, L"SoundForkReadOnlyControlTest", L"SoundFork UI preview (read-only)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 620, nullptr, nullptr,
        GetModuleHandleW(nullptr), &state);
    if (window == nullptr) throw std::runtime_error("preview window failed");
    struct WindowEnd { HWND value; ~WindowEnd() { if (IsWindow(value)) DestroyWindow(value); } } window_end{window};
    refresh_obs_status(&state);
    create_controls(&state);
    rebuild_menu(&state);
    populate_backend_combo(&state);
    populate_endpoint_combo(&state);
    update_status(&state);
    if (interactive) {
      ShowWindow(window, SW_SHOWNORMAL);
      SetTimer(window, kRefreshTimer, 1000, nullptr);
      MSG message{};
      while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (state.diagnostic_window && IsDialogMessageW(state.diagnostic_window, &message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
      }
    } else {
      std::array<wchar_t, 32> class_name{};
      GetClassNameW(state.status, class_name.data(), static_cast<int>(class_name.size()));
      require_ui(std::wstring_view(class_name.data()) == L"Static", "main status is not compact static text");
      require_ui((GetWindowLongPtrW(state.status, GWL_STYLE) & WS_VSCROLL) == 0, "main status still scrolls");
      SendMessageW(window, kAutomaticUpdateMessage, 0, 0);
      require_ui(!state.update_check_in_progress && !state.automatic_update_considered,
          "hidden/disabled control started an automatic update check");
      static std::atomic<int> update_calls{0};
      const auto fake_check = +[]() noexcept -> UpdateResult { ++update_calls; return {}; };
      begin_automatic_update_once(&state, fake_check);
      require_ui(!state.automatic_update_considered, "disabled preference was ignored");
      state.automatic_updates_enabled = true;
      begin_automatic_update_once(&state, fake_check);
      begin_automatic_update_once(&state, fake_check);
      require_ui(state.silent_update_request, "background request was not silent");
      if (state.update_thread.joinable()) state.update_thread.join();
      require_ui(update_calls == 1, "automatic update did not run exactly once");
      SendMessageW(window, kUpdateResultMessage, 0, 0);
      require_ui(!state.update_check_in_progress, "silent failure did not complete");
      state.automatic_updates_enabled = false;
      MSG pending{};
      while (PeekMessageW(&pending, window, kUpdateResultMessage, kUpdateResultMessage, PM_REMOVE)) {}
      if (state.startup_checkbox == nullptr) throw std::runtime_error("startup checkbox missing");
      const HWND startup_checkbox = state.startup_checkbox;
      require_ui(GetDlgItem(window, kStartupRelay) == startup_checkbox, "startup checkbox is not on main window");
      const auto expected_startup = soundfork::control::RelaySettings{}.startup_preferred(executable_path())
          ? BST_CHECKED : BST_UNCHECKED;
      require_ui(SendMessageW(state.startup_checkbox, BM_GETCHECK, 0, 0) == expected_startup,
          "startup does not reflect the saved preference");
      SendMessageW(state.startup_checkbox, BM_SETCHECK,
          expected_startup == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
      update_status(&state);
      require_ui(SendMessageW(state.startup_checkbox, BM_GETCHECK, 0, 0) == expected_startup,
          "checkbox did not synchronize with saved preference");
      require_ui(GetMenuState(GetSubMenu(GetMenu(window), 0), kStartupRelay, MF_BYCOMMAND) == static_cast<UINT>(-1),
          "startup option still duplicated in menu");
      MoveWindow(window, 0, 0, 720, 600, FALSE);
      RECT client{}, note{};
      GetClientRect(window, &client);
      GetWindowRect(state.note, &note);
      MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&note), 2);
      require_ui(note.right <= client.right && note.bottom <= client.bottom, "compact layout clipped at minimum size");
      RECT mode_rect{}, device_rect{}, buffer_rect{}, group_rect{};
      GetWindowRect(state.relay_mode_combo, &mode_rect);
      GetWindowRect(state.endpoint_combo, &device_rect);
      GetWindowRect(state.relay_buffer, &buffer_rect);
      GetWindowRect(state.forwarding_group, &group_rect);
      require_ui(mode_rect.left == device_rect.left && device_rect.left == buffer_rect.left,
          "forwarding fields are not aligned");
      require_ui(mode_rect.bottom < device_rect.top && device_rect.bottom < buffer_rect.top,
          "forwarding rows overlap");
      require_ui(buffer_rect.right < group_rect.right && mode_rect.top > group_rect.top,
          "forwarding fields escape their group");
      for (const auto language : {UiLanguage::korean, UiLanguage::english}) {
        apply_language(&state, language, false);
        std::vector<wchar_t> checkbox_text(128);
        GetWindowTextW(startup_checkbox, checkbox_text.data(), static_cast<int>(checkbox_text.size()));
        require_ui(std::wstring_view(checkbox_text.data()) == startup_strings(language).menu,
            "startup checkbox translation wrong");
        const auto& strings = soundfork::control::diagnostic_strings(language == UiLanguage::korean);
        std::array<wchar_t, 64> menu_text{};
        require_ui(GetMenuStringW(GetSubMenu(GetMenu(window), 1), kShowDiagnostics, menu_text.data(),
            static_cast<int>(menu_text.size()), MF_BYCOMMAND) != 0, "diagnostics menu missing");
        require_ui(std::wstring_view(menu_text.data()) == strings.title, "diagnostics menu translation wrong");
        require_ui(GetMenuStringW(GetSubMenu(GetMenu(window), 1), kOpenLogs, menu_text.data(),
            static_cast<int>(menu_text.size()), MF_BYCOMMAND) != 0, "log-folder menu missing");
        require_ui(std::wstring_view(menu_text.data()) == strings.logs, "log-folder menu translation wrong");
        HWND diagnostic = CreateWindowExW(WS_EX_CONTROLPARENT, kDiagnosticClass, strings.title,
            WS_OVERLAPPEDWINDOW, 0, 0, 740, 480, window, nullptr, GetModuleHandleW(nullptr), &state);
        if (diagnostic == nullptr || !state.diagnostic_copy || !state.diagnostic_close)
          throw std::runtime_error("diagnostic controls missing");
        update_status(&state);
        const HWND diagnostic_text = state.diagnostic_text;
        if (diagnostic_text == nullptr) throw std::runtime_error("diagnostic text missing");
        require_ui(GetWindowTextLengthW(diagnostic_text) > 0, "diagnostics empty");
        require_ui(state.diagnostic_snapshot.starts_with(L"SoundFork v"), "diagnostics version missing");
        apply_language(&state, language == UiLanguage::korean ? UiLanguage::english : UiLanguage::korean, false);
        SendMessageW(diagnostic, WM_COMMAND, kCloseDiagnostics, 0);
        require_ui(state.diagnostic_window == nullptr && state.diagnostic_text == nullptr,
            "closing diagnostics did not clean up handles");
      }
      DestroyWindow(window);
      std::cout << "Native control layout, diagnostics lifecycle and language refresh passed\n";
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
