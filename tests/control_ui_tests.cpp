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
    state.language = UiLanguage::korean;
    state.force_exit = true;  // Closing this test host never touches the installed relay.
    HWND window = CreateWindowExW(0, L"SoundForkReadOnlyControlTest", L"SoundFork UI preview (read-only)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 450, nullptr, nullptr,
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
      if (state.startup_checkbox == nullptr) throw std::runtime_error("startup checkbox missing");
      const HWND startup_checkbox = state.startup_checkbox;
      require_ui(GetDlgItem(window, kStartupRelay) == startup_checkbox, "startup checkbox is not on main window");
      require_ui(SendMessageW(state.startup_checkbox, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
          "startup is not off for an unregistered test executable");
      SendMessageW(state.startup_checkbox, BM_SETCHECK, BST_CHECKED, 0);
      update_status(&state);
      require_ui(SendMessageW(state.startup_checkbox, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
          "checkbox did not synchronize with saved preference");
      require_ui(GetMenuState(GetSubMenu(GetMenu(window), 0), kStartupRelay, MF_BYCOMMAND) == static_cast<UINT>(-1),
          "startup option still duplicated in menu");
      MoveWindow(window, 0, 0, 720, 430, FALSE);
      RECT client{}, note{};
      GetClientRect(window, &client);
      GetWindowRect(state.note, &note);
      MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&note), 2);
      require_ui(note.right <= client.right && note.bottom <= client.bottom, "compact layout clipped at minimum size");
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
