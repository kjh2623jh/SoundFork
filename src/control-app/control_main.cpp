#include <windows.h>
#include <commctrl.h>
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "capture-backend/relay_control.hpp"
#include "capture-backend/relay_preferences.hpp"
#include "capture-backend/channel_mapping.hpp"
#include "capture-backend/shared_ring.hpp"
#include "control-app/resource.h"
#include "control-app/obs_plugin_manager.hpp"
#include "control-app/obs_plugin_strings.hpp"
#include "control-app/update_checker.hpp"
#include "control-app/update_preferences.hpp"
#include "audio-bridge/windows_output_settings.hpp"
#include "control-app/relay_settings.hpp"
#include "control-app/control_status.hpp"
#include "soundfork_version.hpp"
#include "diagnostics/async_log.hpp"

namespace {

namespace obs_plugin = soundfork::control::obs_plugin;
namespace diagnostics = soundfork::diagnostics;

using Microsoft::WRL::ComPtr;
using soundfork::control::UpdateResult;
using soundfork::control::UpdateStatus;
using soundfork::control::check_for_updates;

constexpr wchar_t kWindowClass[] = L"SoundForkControlWindow";
constexpr wchar_t kDiagnosticClass[] = L"SoundForkDiagnosticWindow";
constexpr wchar_t kSoundForkClsid[] =
    L"{A04A0128-E43C-4D08-B701-87EC1A847A89}";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kUpdateResultMessage = WM_APP + 1;
constexpr UINT kAutomaticUpdateMessage = WM_APP + 20;
constexpr int kAutomaticUpdateNotifications = 2300;
constexpr UINT kTrayCallbackMessage = WM_APP + 2;
constexpr UINT kObsPluginResultMessage = WM_APP + 3;
constexpr UINT kShowControlMessage = WM_APP + 4;
constexpr UINT kStartSavedRelayMessage = WM_APP + 5;
constexpr UINT kTrayIconId = 1;

constexpr int kBackendCombo = 1001;
constexpr int kApplyBackend = 1002;
constexpr int kEndpointCombo = 1003;
constexpr int kToggleRelay = 1004;
constexpr int kRefresh = 1005;
constexpr int kStatus = 1006;
constexpr int kGetVbCable = 1007;
constexpr int kLanguageEnglish = 2001;
constexpr int kLanguageKorean = 2002;
constexpr int kStartupRelay = 2003;
constexpr int kRelayModeCombo = 2004;
constexpr int kRelayEnabled = 2005;
constexpr int kRelayBuffer = 2006;
constexpr int kChannels = 4101;
constexpr int kCustomChannels = 4102;
constexpr int kDisplayLanguage = 4103;
constexpr int kNavigate = 4200;
constexpr int kPageCount = 7;
constexpr wchar_t kChannelDialogClass[] = L"SoundForkChannelDialog";
constexpr DWORD kRelayBuffers[] = {0, 128, 256, 480, 960, 1920, 3840, 9600};
constexpr int kCheckForUpdates = 2101;
constexpr int kOpenProject = 2102;
constexpr int kShowAbout = 2103;
constexpr int kShowDiagnostics = 2104;
constexpr int kOpenLogs = 2105;
constexpr int kReportBug = 2106;
constexpr int kCopyReport = 2107;
constexpr int kCopyDiagnostics = 2401;
constexpr int kCloseDiagnostics = 2402;
constexpr int kTrayOpen = 2201;
constexpr int kTrayToggleRelay = 2202;
constexpr int kTrayExitControl = 2203;
constexpr int kTrayStopAndExit = 2204;
constexpr int kObsInstalledStatus = 2301;
constexpr int kObsBundledStatus = 2302;
constexpr int kObsInstall = 2303;
constexpr int kObsRemove = 2304;
constexpr int kObsInformation = 2305;
constexpr wchar_t kVbCableUrl[] = L"https://vb-audio.com/Cable/";
constexpr wchar_t kProjectUrl[] = L"https://github.com/kjh2623jh/SoundFork";
constexpr wchar_t kReleasesUrl[] =
    L"https://github.com/kjh2623jh/SoundFork/releases";

struct RegistryKey {
  HKEY value = nullptr;
  ~RegistryKey() {
    if (value != nullptr) {
      RegCloseKey(value);
    }
  }
};

struct AsioDriver {
  std::wstring name;
  std::wstring clsid;
};

enum class EndpointKind {
  other,
  vb_cable,
  voicemeeter,
};

enum class UiLanguage {
  english,
  korean,
};

struct UiStrings {
  const wchar_t* window_title;
  const wchar_t* settings_menu;
  const wchar_t* language_menu;
  const wchar_t* help_menu;
  const wchar_t* check_for_updates;
  const wchar_t* open_project;
  const wchar_t* about;
  const wchar_t* update_available;
  const wchar_t* update_current;
  const wchar_t* update_open_question;
  const wchar_t* update_not_available;
  const wchar_t* update_failed;
  const wchar_t* open_project_failed;
  const wchar_t* about_license;
  const wchar_t* tray_open;
  const wchar_t* tray_exit_control;
  const wchar_t* tray_stop_and_exit;
  const wchar_t* tray_hidden_title;
  const wchar_t* tray_hidden_message;
  const wchar_t* backend_label;
  const wchar_t* apply;
  const wchar_t* endpoint_label;
  const wchar_t* start_relay;
  const wchar_t* stop_relay;
  const wchar_t* refresh_devices;
  const wchar_t* get_vb_cable;
  const wchar_t* vb_cable_detected;
  const wchar_t* note;
  const wchar_t* recommended;
  const wchar_t* supported;
  const wchar_t* ready;
  const wchar_t* not_ready;
  const wchar_t* not_configured;
  const wchar_t* not_registered;
  const wchar_t* asio_driver;
  const wchar_t* backend;
  const wchar_t* driver_dll;
  const wchar_t* asio_stream;
  const wchar_t* active;
  const wchar_t* inactive;
  const wchar_t* format;
  const wchar_t* channels;
  const wchar_t* frames_per_block;
  const wchar_t* published_consumed;
  const wchar_t* dropped_blocks;
  const wchar_t* capture_consumer;
  const wchar_t* none;
  const wchar_t* broadcast;
  const wchar_t* broadcast_not_ready;
  const wchar_t* discord_relay;
  const wchar_t* running;
  const wchar_t* stopped;
  const wchar_t* discord_mode;
  const wchar_t* application_share_note;
  const wchar_t* relay_missing;
  const wchar_t* relay_start_failed;
  const wchar_t* relay_stop_missing;
  const wchar_t* relay_stop_failed;
  const wchar_t* select_backend;
  const wchar_t* backend_updated;
  const wchar_t* backend_update_failed;
  const wchar_t* another_consumer;
  const wchar_t* select_endpoint;
  const wchar_t* open_vb_cable_failed;
  const wchar_t* com_initialization_failed;
  const wchar_t* backend_missing;
  const wchar_t* backend_access_denied;
  const wchar_t* backend_write_failed;
};

const UiStrings& ui_strings(UiLanguage language) {
  static constexpr UiStrings english{
      L"SoundFork Control",
      L"Settings",
      L"Language",
      L"Help",
      L"Check for updates...",
      L"Open GitHub project",
      L"About SoundFork",
      L"A new SoundFork version is available: ",
      L"Current version: ",
      L"\n\nOpen the release download page?",
      L"You already have the latest SoundFork version.",
      L"Could not check for updates. Check your internet connection and try again.",
      L"Could not open the SoundFork project page.",
      L"Windows x64\nGPL-3.0-only\n\nLow-latency ASIO loopback bridge",
      L"Open SoundFork Control",
      L"Exit SoundFork Control",
      L"Stop forwarding and exit",
      L"SoundFork is still running",
      L"Audio forwarding is still running in the notification area.",
      L"ASIO driver to use",
      L"Apply",
      L"Broadcast audio device (virtual device)",
      L"Start forwarding",
      L"Stop forwarding",
      L"Refresh devices",
      L"Get VB-CABLE",
      L"VB-CABLE detected",
      L"Discord: entire-screen sharing only. OBS plugin capture needs no forwarding.",
      L"Recommended",
      L"Supported",
      L"Ready",
      L"Not ready",
      L"Not configured",
      L"Not registered",
      L"ASIO driver",
      L"Selected ASIO driver",
      L"Driver DLL",
      L"Audio copy connection",
      L"Active",
      L"Inactive",
      L"Format",
      L"channels",
      L"frames/block",
      L"Published/consumed blocks",
      L"Dropped capture blocks",
      L"Capture consumer",
      L"None",
      L"Broadcast",
      L"Not ready - install VB-CABLE or select VoiceMeeter",
      L"Forwarding to virtual device",
      L"Running",
      L"Stopped",
      L"External relay: Entire-screen share only",
      L"Audio is forwarded by a separate process. In Discord, share the entire screen.",
      L"soundfork_discord_relay.exe was not found next to the control app.",
      L"Could not start audio forwarding (Windows error ",
      L"The audio forwarding stop channel was not found.",
      L"Could not signal audio forwarding to stop.",
      L"Select an ASIO driver first.",
      L"ASIO driver updated. Restart the application if it is already open.",
      L"ASIO driver change was cancelled or failed.",
      L"Another capture consumer is active. Stop soundfork_capture_monitor or the other consumer first.",
      L"Select a virtual broadcast audio device first.",
      L"Could not open the official VB-CABLE download page.",
      L"COM initialization failed.",
      L"The selected ASIO driver is no longer registered.",
      L"SoundFork is not registered or administrator access was denied.",
      L"Could not change the ASIO driver SoundFork uses."};
  static constexpr UiStrings korean{
      L"SoundFork 제어판",
      L"설정",
      L"언어",
      L"도움말",
      L"업데이트 확인...",
      L"GitHub 프로젝트 열기",
      L"SoundFork 정보",
      L"새 SoundFork 버전이 있습니다: ",
      L"현재 버전: ",
      L"\n\n릴리스 다운로드 페이지를 여시겠습니까?",
      L"현재 최신 SoundFork 버전을 사용하고 있습니다.",
      L"업데이트를 확인할 수 없습니다. 인터넷 연결을 확인한 후 다시 시도하세요.",
      L"SoundFork 프로젝트 페이지를 열 수 없습니다.",
      L"Windows x64\nGPL-3.0-only\n\n저지연 ASIO 루프백 브리지",
      L"SoundFork 제어판 열기",
      L"SoundFork 제어판 종료",
      L"전달 중지 후 종료",
      L"SoundFork가 계속 실행 중입니다",
      L"오디오 전달이 알림 영역에서 계속 실행 중입니다.",
      L"사용할 ASIO 드라이버",
      L"적용",
      L"방송용 오디오 장치 (가상 장치 선택)",
      L"전달 시작",
      L"전달 중지",
      L"장치 새로 고침",
      L"VB-CABLE 받기",
      L"VB-CABLE 감지됨",
      L"Discord는 전체 화면 공유만 지원합니다. OBS 플러그인은 전달을 켜지 않아도 됩니다.",
      L"권장",
      L"지원됨",
      L"준비됨",
      L"준비되지 않음",
      L"설정되지 않음",
      L"등록되지 않음",
      L"ASIO 드라이버",
      L"선택한 ASIO 드라이버",
      L"드라이버 DLL",
      L"오디오 복제 연결",
      L"활성",
      L"비활성",
      L"포맷",
      L"채널",
      L"프레임/블록",
      L"발행/소비 블록",
      L"드롭된 캡처 블록",
      L"캡처 소비자",
      L"없음",
      L"방송",
      L"준비되지 않음 - VB-CABLE을 설치하거나 VoiceMeeter를 선택하세요",
      L"가상 장치로 오디오 전달",
      L"실행 중",
      L"중지됨",
      L"외부 릴레이: 전체 화면 공유 전용",
      L"별도 프로세스에서 오디오를 전달합니다. Discord에서 전체 화면을 공유하세요.",
      L"제어판과 같은 폴더에서 soundfork_discord_relay.exe를 찾을 수 없습니다.",
      L"오디오 전달을 시작할 수 없습니다 (Windows 오류 ",
      L"오디오 전달 종료 채널을 찾을 수 없습니다.",
      L"오디오 전달에 종료 신호를 보낼 수 없습니다.",
      L"먼저 ASIO 드라이버를 선택하세요.",
      L"ASIO 드라이버를 변경했습니다. 애플리케이션이 실행 중이라면 다시 시작하세요.",
      L"ASIO 드라이버 변경이 취소됐거나 실패했습니다.",
      L"다른 캡처 소비자가 실행 중입니다. soundfork_capture_monitor 또는 다른 소비자를 먼저 종료하세요.",
      L"먼저 방송용 가상 오디오 장치를 선택하세요.",
      L"VB-CABLE 공식 다운로드 페이지를 열 수 없습니다.",
      L"COM 초기화에 실패했습니다.",
      L"선택한 ASIO 드라이버가 더 이상 등록되어 있지 않습니다.",
      L"SoundFork가 등록되어 있지 않거나 관리자 권한이 거부됐습니다.",
      L"SoundFork가 사용할 ASIO 드라이버를 변경할 수 없습니다."};
  return language == UiLanguage::korean ? korean : english;
}

struct Endpoint {
  std::wstring name;
  std::wstring id;
  EndpointKind kind = EndpointKind::other;
  bool is_default = false;
};

struct RegistrationStatus {
  bool asio_registered = false;
  bool com_registered = false;
  bool dll_exists = false;
  std::wstring backend_clsid;
  std::wstring driver_dll;
};

struct PanelControls {
  HWND channels = nullptr, channel_label = nullptr, custom = nullptr;
  HWND obs_title = nullptr, obs_status = nullptr, obs_install = nullptr, obs_remove = nullptr,
       obs_usage = nullptr;
  HWND obs_version_label = nullptr, obs_bundle_label = nullptr, obs_bundle = nullptr,
       obs_actions = nullptr, obs_guide = nullptr;
  HWND obs_details = nullptr;
  HWND settings_title = nullptr, language_label = nullptr, language = nullptr, updates = nullptr,
       settings_note = nullptr;
  HWND playback_note = nullptr, buffer_group = nullptr, buffer_note = nullptr,
       playback_info = nullptr;
  HWND information_group = nullptr, information = nullptr, diagnostic_button = nullptr,
       logs_button = nullptr;
  HWND program_group = nullptr, program = nullptr, program_name = nullptr, support_group = nullptr;
  HWND update_group = nullptr, update_button = nullptr, bug_button = nullptr,
       project_button = nullptr;
  HFONT heading_font = nullptr;
  soundfork::capture::ChannelPair pair{};
  std::array<std::vector<HWND>, kPageCount> pages;
  int page = 0;
  bool ready = false;
};

struct AppState {
  PanelControls panel;
  HWND window = nullptr;
  HWND backend_combo = nullptr;
  HWND endpoint_combo = nullptr;
  HWND relay_button = nullptr;
  HWND vb_cable_button = nullptr;
  HWND status = nullptr;
  HWND startup_checkbox = nullptr;
  HWND relay_mode_label = nullptr;
  HWND relay_mode_combo = nullptr;
  HWND relay_enabled = nullptr;
  HWND relay_buffer = nullptr;
  HWND output_group = nullptr;
  HWND forwarding_group = nullptr;
  HWND buffer_label = nullptr;
  HWND diagnostic_window = nullptr;
  HWND diagnostic_text = nullptr;
  HWND diagnostic_copy = nullptr;
  HWND diagnostic_close = nullptr;
  std::wstring diagnostic_snapshot;
  HWND apply_button = nullptr;
  HWND endpoint_label = nullptr;
  HWND refresh_button = nullptr;
  HWND note = nullptr;
  HFONT font = nullptr;
  UiLanguage language = UiLanguage::english;
  std::vector<AsioDriver> drivers;
  std::vector<Endpoint> endpoints;
  std::atomic<bool> shutting_down{false};
  bool update_check_in_progress = false;
  bool automatic_updates_enabled = true;
  bool automatic_update_considered = false;
  bool silent_update_request = false;
  bool pending_update_notice = false;
  std::mutex update_mutex;
  soundfork::control::UpdateResult update_result;
  std::thread update_thread;
  obs_plugin::Status obs_status;
  bool obs_operation_in_progress = false;
  bool obs_operation_install = false;
  std::atomic<DWORD> obs_result{0};
  std::thread obs_thread;
  HICON tray_icon = nullptr;
  bool tray_icon_added = false;
  bool tray_notification_shown = false;
  bool force_exit = false;
  std::wstring startup_error;
  HANDLE startup_process = nullptr;
  ULONGLONG next_endpoint_probe = 0;
  int logged_status = -1;
  ~AppState() { if (startup_process != nullptr) CloseHandle(startup_process); }
};

struct StartupStrings {
  const wchar_t* menu;
  const wchar_t* save_failed;
  const wchar_t* failed;
  const wchar_t* missing;
  const wchar_t* waiting;
  const wchar_t* enabled;
  const wchar_t* disabled;
  const wchar_t* preserved;
};

struct RelayModeStrings {
  const wchar_t* label;
  const wchar_t* entire_screen;
  const wchar_t* application;
  const wchar_t* starts_with_game;
  const wchar_t* restart_game;
  const wchar_t* application_note;
};

const RelayModeStrings& relay_mode_strings(UiLanguage language) {
  static constexpr RelayModeStrings en{
      L"Audio forwarding mode",
      L"External relay — entire-screen sharing",
      L"Internal relay — application-window sharing supported",
      L"Starts with ASIO",
      L"The internal relay will start the next time the application initializes SoundFork ASIO.",
      L"In Discord, share the application window or the entire screen. The OBS plugin can be used simultaneously."};
  static constexpr RelayModeStrings ko{
      L"오디오 전달 방식",
      L"외부 릴레이 — 전체 화면 공유",
      L"내부 릴레이 — 애플리케이션 창 공유 지원",
      L"ASIO 연결 시 시작",
      L"내부 릴레이는 애플리케이션이 다음에 SoundFork ASIO를 초기화할 때 시작됩니다.",
      L"Discord에서 애플리케이션 창 또는 전체 화면을 공유하세요. OBS 플러그인도 함께 사용할 수 있습니다."};
  return language == UiLanguage::korean ? ko : en;
}
const StartupStrings& startup_strings(UiLanguage language) {
  static constexpr StartupStrings en{
    L"Start audio forwarding at Windows sign-in", L"Could not save the audio forwarding setting. Windows error: ",
    L"Automatic audio forwarding failed. ", L"Saved virtual device is unavailable; no other device was selected.",
    L"Waiting for the saved virtual device or ASIO audio", L"Windows sign-in startup: On",
    L"Windows sign-in startup: Off", L"Windows sign-in startup: On (used for entire-screen sharing)"};
  static constexpr StartupStrings ko{
    L"Windows 로그인 시 오디오 전달 시작", L"오디오 전달 설정을 저장하지 못했습니다. Windows 오류: ",
    L"자동 오디오 전달에 실패했습니다. ", L"저장한 가상 장치를 찾을 수 없습니다. 다른 장치로 전환하지 않습니다.",
    L"저장한 가상 장치 또는 ASIO 오디오 대기 중", L"Windows 로그인 자동 시작: 켜짐",
    L"Windows 로그인 자동 시작: 꺼짐", L"Windows 로그인 자동 시작: 켜짐 (전체 화면 공유에서 적용)"};
  return language == UiLanguage::korean ? ko : en;
}

UINT taskbar_created_message = 0;

std::wstring read_registry_string(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) !=
          ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
    return {};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegQueryValueExW(
          key,
          name,
          nullptr,
          &type,
          reinterpret_cast<BYTE*>(value.data()),
          &bytes) != ERROR_SUCCESS) {
    return {};
  }
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
}

bool equals_ignore_case(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() &&
         std::equal(
             left.begin(),
             left.end(),
             right.begin(),
             [](wchar_t a, wchar_t b) {
               return std::towlower(a) == std::towlower(b);
             });
}

std::wstring lowercase(std::wstring_view value) {
  std::wstring result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](wchar_t item) {
    return static_cast<wchar_t>(std::towlower(item));
  });
  return result;
}

bool parse_ui_language(std::wstring_view value, UiLanguage* language) {
  if (equals_ignore_case(value, L"english") ||
      equals_ignore_case(value, L"en")) {
    *language = UiLanguage::english;
    return true;
  }
  if (equals_ignore_case(value, L"korean") ||
      equals_ignore_case(value, L"ko")) {
    *language = UiLanguage::korean;
    return true;
  }
  return false;
}

const wchar_t* ui_language_name(UiLanguage language) {
  return language == UiLanguage::korean ? L"korean" : L"english";
}

UiLanguage language_from_windows_ui(LANGID language_id) {
  return PRIMARYLANGID(language_id) == LANG_KOREAN ? UiLanguage::korean
                                                    : UiLanguage::english;
}

bool read_saved_language(UiLanguage* language) {
  RegistryKey key;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\SoundFork",
          0,
          KEY_READ,
          &key.value) != ERROR_SUCCESS) {
    return false;
  }
  return parse_ui_language(read_registry_string(key.value, L"Language"), language);
}

bool save_language(UiLanguage language, bool only_if_missing = false) {
  if (only_if_missing) {
    UiLanguage existing{};
    if (read_saved_language(&existing)) {
      return true;
    }
  }
  RegistryKey key;
  if (RegCreateKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\SoundFork",
          0,
          nullptr,
          0,
          KEY_SET_VALUE,
          nullptr,
          &key.value,
          nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const wchar_t* name = ui_language_name(language);
  return RegSetValueExW(
             key.value,
             L"Language",
             0,
             REG_SZ,
             reinterpret_cast<const BYTE*>(name),
             static_cast<DWORD>((std::wcslen(name) + 1) * sizeof(wchar_t))) ==
         ERROR_SUCCESS;
}

UiLanguage load_ui_language() {
  UiLanguage language{};
  if (read_saved_language(&language)) {
    return language;
  }
  return language_from_windows_ui(GetUserDefaultUILanguage());
}

EndpointKind classify_endpoint(std::wstring_view name) {
  const std::wstring lower = lowercase(name);
  if (lower.find(L"cable input") != std::wstring::npos ||
      lower.find(L"vb-audio virtual cable") != std::wstring::npos ||
      lower.find(L"vb-audio cable") != std::wstring::npos) {
    return EndpointKind::vb_cable;
  }
  if (lower.find(L"voicemeeter") != std::wstring::npos) {
    return EndpointKind::voicemeeter;
  }
  return EndpointKind::other;
}

const wchar_t* endpoint_kind_name(EndpointKind kind) {
  switch (kind) {
    case EndpointKind::vb_cable:
      return L"VB-CABLE";
    case EndpointKind::voicemeeter:
      return L"VoiceMeeter";
    default:
      return L"Unsupported endpoint";
  }
}

bool is_broadcast_endpoint(const Endpoint& endpoint) {
  return endpoint.kind != EndpointKind::other;
}

std::vector<AsioDriver> enumerate_asio_drivers() {
  std::vector<AsioDriver> drivers;
  RegistryKey root;
  if (RegOpenKeyExW(
          HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\ASIO",
          0,
          KEY_READ | KEY_WOW64_64KEY,
          &root.value) != ERROR_SUCCESS) {
    return drivers;
  }

  DWORD subkey_count = 0;
  DWORD maximum_name = 0;
  if (RegQueryInfoKeyW(
          root.value,
          nullptr,
          nullptr,
          nullptr,
          &subkey_count,
          &maximum_name,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          nullptr) != ERROR_SUCCESS) {
    return drivers;
  }
  std::vector<wchar_t> name(maximum_name + 2, L'\0');
  for (DWORD index = 0; index < subkey_count; ++index) {
    DWORD length = static_cast<DWORD>(name.size());
    if (RegEnumKeyExW(
            root.value,
            index,
            name.data(),
            &length,
            nullptr,
            nullptr,
            nullptr,
            nullptr) != ERROR_SUCCESS) {
      continue;
    }
    const std::wstring driver_name(name.data(), length);
    RegistryKey driver_key;
    if (RegOpenKeyExW(
            root.value,
            driver_name.c_str(),
            0,
            KEY_READ,
            &driver_key.value) != ERROR_SUCCESS) {
      continue;
    }
    std::wstring clsid = read_registry_string(driver_key.value, L"CLSID");
    if (clsid.empty() || equals_ignore_case(clsid, kSoundForkClsid)) {
      continue;
    }
    drivers.push_back({driver_name, std::move(clsid)});
  }
  std::sort(
      drivers.begin(), drivers.end(), [](const AsioDriver& a, const AsioDriver& b) {
        return a.name < b.name;
      });
  return drivers;
}

RegistrationStatus query_registration() {
  RegistrationStatus status;
  RegistryKey asio_key;
  if (RegOpenKeyExW(
          HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\ASIO\\SoundFork",
          0,
          KEY_READ | KEY_WOW64_64KEY,
          &asio_key.value) == ERROR_SUCCESS) {
    status.asio_registered = equals_ignore_case(
        read_registry_string(asio_key.value, L"CLSID"), kSoundForkClsid);
    status.backend_clsid =
        read_registry_string(asio_key.value, L"BackendCLSID");
  }

  const std::wstring com_path =
      std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kSoundForkClsid +
      L"\\InprocServer32";
  RegistryKey com_key;
  if (RegOpenKeyExW(
          HKEY_LOCAL_MACHINE,
          com_path.c_str(),
          0,
          KEY_READ | KEY_WOW64_64KEY,
          &com_key.value) == ERROR_SUCCESS) {
    status.driver_dll = read_registry_string(com_key.value, nullptr);
    status.com_registered = !status.driver_dll.empty();
    status.dll_exists = status.com_registered &&
                        GetFileAttributesW(status.driver_dll.c_str()) !=
                            INVALID_FILE_ATTRIBUTES;
  }
  std::wstring endpoint;
  if(soundfork::read_windows_output(&endpoint) == S_OK) status.backend_clsid = L"wasapi:" + endpoint;
  return status;
}

std::vector<Endpoint> enumerate_endpoints() {
  std::vector<Endpoint> endpoints;
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(
          __uuidof(MMDeviceEnumerator),
          nullptr,
          CLSCTX_ALL,
          IID_PPV_ARGS(&enumerator)))) {
    return endpoints;
  }
  std::wstring default_id;
  ComPtr<IMMDevice> default_device;
  LPWSTR default_device_id = nullptr;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device)) &&
      SUCCEEDED(default_device->GetId(&default_device_id)) && default_device_id != nullptr) {
    default_id = default_device_id;
    CoTaskMemFree(default_device_id);
  }
  ComPtr<IMMDeviceCollection> collection;
  if (FAILED(enumerator->EnumAudioEndpoints(
          eRender, DEVICE_STATE_ACTIVE, &collection))) {
    return endpoints;
  }
  UINT count = 0;
  if (FAILED(collection->GetCount(&count))) {
    return endpoints;
  }
  for (UINT index = 0; index < count; ++index) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(index, &device))) {
      continue;
    }
    Endpoint endpoint;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
      endpoint.id = id;
      CoTaskMemFree(id);
    }
    ComPtr<IPropertyStore> properties;
    PROPVARIANT name;
    PropVariantInit(&name);
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
        SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
        name.vt == VT_LPWSTR && name.pwszVal != nullptr) {
      endpoint.name = name.pwszVal;
    }
    PropVariantClear(&name);
    if (!endpoint.id.empty()) {
      if (endpoint.name.empty()) {
        endpoint.name = endpoint.id;
      }
      endpoint.kind = classify_endpoint(endpoint.name);
      endpoint.is_default = equals_ignore_case(endpoint.id, default_id);
      endpoints.push_back(std::move(endpoint));
    }
  }
  std::sort(
      endpoints.begin(), endpoints.end(), [](const Endpoint& a, const Endpoint& b) {
        return a.name < b.name;
      });
  return endpoints;
}

bool has_broadcast_endpoint() {
  const std::vector<Endpoint> endpoints = enumerate_endpoints();
  return std::any_of(
      endpoints.begin(), endpoints.end(), [](const Endpoint& endpoint) {
        return is_broadcast_endpoint(endpoint);
      });
}

std::wstring find_driver_name(
    const std::vector<AsioDriver>& drivers, std::wstring_view clsid) {
  for (const AsioDriver& driver : drivers) {
    if (equals_ignore_case(driver.clsid, clsid)) {
      return driver.name;
    }
  }
  return {};
}

bool relay_is_running() {
  HANDLE mutex = OpenMutexW(
      SYNCHRONIZE, FALSE, soundfork::capture::kRelaySingletonName);
  if (mutex == nullptr) {
    return false;
  }
  CloseHandle(mutex);
  return true;
}

bool capture_consumer_is_active() {
  soundfork::capture::SharedRingObserver observer;
  soundfork::capture::StreamStatus status;
  if (!observer.open() || !observer.snapshot(&status) || !status.consumer_active) {
    return false;
  }
  // The capture hub owns the input ring when OBS is already capturing. Relay
  // joins a separate hub output and is allowed to start in that case.
  HANDLE ready = OpenEventW(SYNCHRONIZE, FALSE,
      L"Local\\SoundFork.CaptureHub.v2.Ready");
  if (ready == nullptr) return true;
  const bool hub_ready = WaitForSingleObject(ready, 0) == WAIT_OBJECT_0;
  CloseHandle(ready);
  return !hub_ready;
}

std::wstring executable_path() {
  std::vector<wchar_t> buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  return length == 0 ? std::wstring() : std::wstring(buffer.data(), length);
}

std::wstring executable_directory() {
  std::wstring path = executable_path();
  const std::size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

std::wstring quote_argument(std::wstring_view argument) {
  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
    } else if (character == L'\"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'\"');
      backslashes = 0;
    } else {
      result.append(backslashes, L'\\');
      backslashes = 0;
      result.push_back(character);
    }
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

bool start_relay(
    const Endpoint& endpoint, UiLanguage language, std::wstring* error, HANDLE* child = nullptr) {
  if (!soundfork::capture::relay_enabled()) {
    *error = language == UiLanguage::korean ? L"오디오 전달 사용을 켜세요." : L"Enable audio forwarding first.";
    return false;
  }
  const UiStrings& strings = ui_strings(language);
  diagnostics::log().write(diagnostics::Level::info, "relay_start_requested");
  const std::wstring relay_path =
      executable_directory() + L"\\soundfork_discord_relay.exe";
  if (GetFileAttributesW(relay_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    diagnostics::log().write(diagnostics::Level::error, "relay_binary_missing");
    *error = strings.relay_missing;
    return false;
  }
  std::wstring command = quote_argument(relay_path) + L" --endpoint-id " +
                         quote_argument(endpoint.id) +
                         L" --system-share --persistent";
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(
          relay_path.c_str(),
          mutable_command.data(),
          nullptr,
          nullptr,
          FALSE,
          CREATE_NO_WINDOW,
          nullptr,
          executable_directory().c_str(),
          &startup,
          &process)) {
    const auto code = GetLastError();
    diagnostics::log().write(diagnostics::Level::error, "relay_spawn_failed", "win32=" + std::to_string(code));
    *error = std::wstring(strings.relay_start_failed) +
             std::to_wstring(code) + L").";
    return false;
  }
  CloseHandle(process.hThread);
  diagnostics::log().write(diagnostics::Level::info, "relay_spawned", "child_pid=" + std::to_string(process.dwProcessId));
  if (child != nullptr) *child = process.hProcess;
  else CloseHandle(process.hProcess);
  return true;
}

bool stop_relay(UiLanguage language, std::wstring* error) {
  const UiStrings& strings = ui_strings(language);
  diagnostics::log().write(diagnostics::Level::info, "relay_stop_requested");
  HANDLE event = OpenEventW(
      EVENT_MODIFY_STATE, FALSE, soundfork::capture::kRelayStopEventName);
  if (event == nullptr) {
    diagnostics::log().write(diagnostics::Level::warning, "relay_stop_event_missing");
    *error = strings.relay_stop_missing;
    return false;
  }
  const bool result = SetEvent(event) != FALSE;
  CloseHandle(event);
  if (!result) {
    diagnostics::log().write(diagnostics::Level::error, "relay_stop_signal_failed");
    *error = strings.relay_stop_failed;
  }
  if (result) diagnostics::log().write(diagnostics::Level::info, "relay_stop_signaled");
  return result;
}

bool invoke_x86_registration(bool install, std::wstring_view clsid, std::wstring* error) {
  const auto path = executable_directory() + L"\\x86\\soundfork_register.exe";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    // Developer x64-only builds retain their existing registration behavior.
    return true;
  }
  std::wstring command = quote_argument(path) + (install ? L" --register " + quote_argument(clsid) : L" --unregister");
  std::vector<wchar_t> writable(command.begin(), command.end()); writable.push_back(L'\0');
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(path.c_str(), writable.data(), nullptr, nullptr, FALSE,
      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    *error = L"Could not launch x86 ASIO registration helper."; return false;
  }
  CloseHandle(process.hThread);
  const auto wait = WaitForSingleObject(process.hProcess, 30000);
  DWORD code = 1;
  if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
  else { TerminateProcess(process.hProcess, ERROR_TIMEOUT); WaitForSingleObject(process.hProcess, 1000); }
  CloseHandle(process.hProcess);
  if (code != 0) { *error = L"x86 ASIO registration failed (code " + std::to_wstring(code) + L")."; return false; }
  return true;
}

bool set_backend(
    std::wstring_view requested_clsid,
    std::wstring* error,
    UiLanguage language = UiLanguage::english) {
  const UiStrings& strings = ui_strings(language);
  if(requested_clsid.starts_with(L"wasapi:") || requested_clsid == L"windows") {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if(FAILED(com) && com != RPC_E_CHANGED_MODE) { *error = L"Could not initialize Windows audio."; return false; }
    struct ComScope { bool active; ~ComScope() { if(active) CoUninitialize(); } } scope{SUCCEEDED(com)};
    std::wstring endpoint_id;
    if(requested_clsid == L"windows") {
      (void)soundfork::read_windows_output(&endpoint_id);
      if(endpoint_id.empty()) {
      ComPtr<IMMDeviceEnumerator> enumerator;
      ComPtr<IMMDevice> endpoint;
      LPWSTR id = nullptr;
      if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))) ||
          FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)) || FAILED(endpoint->GetId(&id))) {
        endpoint_id = L"unconfigured";
      } else { endpoint_id = id; CoTaskMemFree(id); }
      }
    } else endpoint_id = requested_clsid.substr(7);
    const auto devices = enumerate_endpoints();
    if(requested_clsid != L"windows" && std::none_of(devices.begin(), devices.end(), [&](const auto& item) { return item.id == endpoint_id; })) {
      *error = L"The selected Windows playback device is unavailable."; return false;
    }
    if(!invoke_x86_registration(true, L"{00000000-0000-0000-0000-000000000001}", error)) return false;
    if(soundfork::save_windows_output(endpoint_id) != ERROR_SUCCESS) {
      *error = L"Could not save the Windows playback device."; return false;
    }
    return true;
  }
  const std::vector<AsioDriver> drivers = enumerate_asio_drivers();
  const auto selected = std::find_if(
      drivers.begin(), drivers.end(), [&](const AsioDriver& driver) {
        return equals_ignore_case(driver.clsid, requested_clsid);
      });
  if (selected == drivers.end()) {
    *error = strings.backend_missing;
    return false;
  }
  if (!invoke_x86_registration(true, selected->clsid, error)) return false;
  RegistryKey key;
  const LSTATUS open_result = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\ASIO\\SoundFork",
      0,
      KEY_SET_VALUE | KEY_WOW64_64KEY,
      &key.value);
  if (open_result != ERROR_SUCCESS) {
    *error = strings.backend_access_denied;
    return false;
  }
  const DWORD bytes = static_cast<DWORD>(
      (selected->clsid.size() + 1) * sizeof(wchar_t));
  if (RegSetValueExW(
          key.value,
          L"BackendCLSID",
          0,
          REG_SZ,
          reinterpret_cast<const BYTE*>(selected->clsid.c_str()),
          bytes) != ERROR_SUCCESS) {
    *error = strings.backend_write_failed;
    return false;
  }
  if(soundfork::save_windows_output(L"") != ERROR_SUCCESS) {
    *error = strings.backend_write_failed; return false;
  }
  return true;
}

using DllRegistrationEntry = HRESULT(STDAPICALLTYPE*)();

bool invoke_adjacent_driver_registration(
    bool register_driver,
    std::wstring_view backend_clsid,
    std::wstring* error) {
  const RegistrationStatus previous = query_registration();
  const std::wstring driver_path =
      executable_directory() + L"\\soundfork_driver.dll";
  if (GetFileAttributesW(driver_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    *error = L"soundfork_driver.dll was not found next to the control app.";
    return false;
  }

  if (register_driver) {
    const std::vector<AsioDriver> drivers = enumerate_asio_drivers();
    const bool backend_is_registered = std::any_of(
        drivers.begin(), drivers.end(), [&](const AsioDriver& driver) {
          return equals_ignore_case(driver.clsid, backend_clsid);
        });
    if (!backend_is_registered && backend_clsid != L"windows" && !backend_clsid.starts_with(L"wasapi:")) {
      *error = L"The requested x64 ASIO backend is not registered.";
      return false;
    }
  }

  HMODULE module = LoadLibraryExW(
      driver_path.c_str(),
      nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (module == nullptr) {
    *error = L"Could not load the adjacent SoundFork driver (Windows error " +
             std::to_wstring(GetLastError()) + L").";
    return false;
  }

  const char* entry_name =
      register_driver ? "DllRegisterServer" : "DllUnregisterServer";
  const auto entry = reinterpret_cast<DllRegistrationEntry>(
      GetProcAddress(module, entry_name));
  if (entry == nullptr) {
    *error = L"The SoundFork driver does not export its registration entry.";
    FreeLibrary(module);
    return false;
  }

  const HRESULT registration_result = entry();
  if (FAILED(registration_result)) {
    std::wostringstream message;
    message << L"SoundFork driver registration failed (HRESULT 0x" << std::hex
            << static_cast<unsigned long>(registration_result) << L").";
    *error = message.str();
    FreeLibrary(module);
    return false;
  }

  if (register_driver && !set_backend(backend_clsid, error)) {
    if (previous.asio_registered && !previous.backend_clsid.empty()) {
      std::wstring ignored_error;
      (void)set_backend(previous.backend_clsid, &ignored_error);
    } else {
      const auto rollback = reinterpret_cast<DllRegistrationEntry>(
          GetProcAddress(module, "DllUnregisterServer"));
      if (rollback != nullptr) {
        (void)rollback();
      }
    }
    FreeLibrary(module);
    return false;
  }

  FreeLibrary(module);
  if (!register_driver) {
    if(!invoke_x86_registration(false, L"", error)) return false;
    return soundfork::save_windows_output(L"") == ERROR_SUCCESS;
  }
  return true;
}

bool elevate_set_backend(HWND owner, std::wstring_view clsid) {
  const std::wstring parameters =
      std::wstring(L"--set-backend ") + quote_argument(clsid);
  SHELLEXECUTEINFOW execute{sizeof(execute)};
  execute.fMask = SEE_MASK_NOCLOSEPROCESS;
  execute.hwnd = owner;
  execute.lpVerb = L"runas";
  const std::wstring path = executable_path();
  execute.lpFile = path.c_str();
  execute.lpParameters = parameters.c_str();
  execute.nShow = SW_HIDE;
  if (!ShellExecuteExW(&execute)) {
    return false;
  }
  if (execute.hProcess == nullptr) {
    return false;
  }
  WaitForSingleObject(execute.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(execute.hProcess, &exit_code);
  CloseHandle(execute.hProcess);
  return exit_code == 0;
}

std::wstring obs_installed_text(const AppState* state) {
  const auto& text = soundfork::control::obs_plugin_strings(state->language == UiLanguage::korean);
  if (!state->obs_status.present) return text.absent;
  if (!state->obs_status.complete) return text.incomplete;
  std::wstring result = state->obs_status.installed_version.empty() ? text.unknown : state->obs_status.installed_version;
  if (state->obs_status.update_required) result += state->language == UiLanguage::korean
      ? L" — 업데이트 필요 (같은 SoundFork 빌드 사용)" : L" — Update required (use the same SoundFork build)";
  return result;
}

void update_obs_controls(AppState* state) {
  auto& panel = state->panel;
  if (!panel.obs_status) return;
  const bool korean = state->language == UiLanguage::korean;
  const auto& text = soundfork::control::obs_plugin_strings(korean);
  auto installed = obs_installed_text(state);
  if (state->obs_status.present && state->obs_status.complete) {
    installed = state->obs_status.installed_version.empty() ? text.unknown
                                                            : state->obs_status.installed_version;
    if (state->obs_status.update_required)
      installed += korean ? L" (업데이트 필요)" : L" (update available)";
  }
  SetWindowTextW(panel.obs_status, installed.c_str());
  SetWindowTextW(panel.obs_bundle, state->obs_status.bundled
                                       ? state->obs_status.bundled_version.c_str()
                                       : text.not_bundled);
  SetWindowTextW(panel.obs_install, state->obs_operation_in_progress
                                        ? text.working
                                        : (state->obs_status.present ? text.update : text.install));
  EnableWindow(panel.obs_install, !state->obs_operation_in_progress && state->obs_status.bundled &&
                                      state->obs_status.safe);
  EnableWindow(panel.obs_remove, !state->obs_operation_in_progress && state->obs_status.present &&
                                     state->obs_status.safe);
}

void refresh_obs_status(AppState* state) {
  state->obs_status = obs_plugin::inspect(obs_plugin::locate(executable_directory()));
  update_obs_controls(state);
}

void start_obs_operation(AppState* state, bool install) {
  if (state->obs_operation_in_progress) return;
  const auto& text = soundfork::control::obs_plugin_strings(state->language == UiLanguage::korean);
  if (obs_plugin::obs_is_running()) {
    MessageBoxW(state->window, text.close_obs, text.menu, MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (!install && MessageBoxW(state->window, text.remove_question, text.menu,
                              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
    return;
  if (state->obs_thread.joinable()) state->obs_thread.join();
  state->obs_operation_in_progress = true;
  state->obs_operation_install = install;
  update_obs_controls(state);
  try {
    const auto executable = executable_path();
    state->obs_thread = std::thread([state, executable, install] {
      DWORD result = static_cast<DWORD>(obs_plugin::Error::file_operation);
      const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      if (SUCCEEDED(com)) {
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        execute.hwnd = state->window;
        execute.lpVerb = L"runas";
        execute.lpFile = executable.c_str();
        execute.lpParameters = install ? L"--install-obs-plugin" : L"--remove-obs-plugin";
        execute.nShow = SW_HIDE;
        if (ShellExecuteExW(&execute)) {
          if (execute.hProcess != nullptr) {
            DWORD wait = WAIT_TIMEOUT;
            while (!state->shutting_down.load(std::memory_order_acquire) &&
                   (wait = WaitForSingleObject(execute.hProcess, 100)) == WAIT_TIMEOUT) {}
            if (wait == WAIT_OBJECT_0) (void)GetExitCodeProcess(execute.hProcess, &result);
            CloseHandle(execute.hProcess);
          }
        } else if (GetLastError() == ERROR_CANCELLED) {
          result = ERROR_CANCELLED;
        }
        CoUninitialize();
      }
      state->obs_result.store(result, std::memory_order_release);
      if (!state->shutting_down.load(std::memory_order_acquire))
        PostMessageW(state->window, kObsPluginResultMessage, 0, 0);
    });
  } catch (...) {
    state->obs_operation_in_progress = false;
    update_obs_controls(state);
    MessageBoxW(state->window, text.failed, text.menu, MB_ICONERROR);
  }
}

void set_control_font(HWND control, HFONT font) {
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

std::wstring playback_endpoint_name(const Endpoint& endpoint, UiLanguage language) {
  auto name = L"Windows — " + endpoint.name;
  if (endpoint.is_default) name += language == UiLanguage::korean
      ? L" (기본 장치)" : L" (Default device)";
  return name;
}

void populate_backend_combo(AppState* state, bool preserve_selection = false) {
  const RegistrationStatus registration = query_registration();
  auto selected_clsid = registration.backend_clsid;
  const auto previous = SendMessageW(state->backend_combo, CB_GETCURSEL, 0, 0);
  if (preserve_selection && previous >= 0 && static_cast<std::size_t>(previous) < state->drivers.size())
    selected_clsid = state->drivers[static_cast<std::size_t>(previous)].clsid;
  state->drivers = enumerate_asio_drivers();
  for(auto& driver : state->drivers) driver.name = L"ASIO — " + driver.name;
  for(const auto& endpoint : enumerate_endpoints())
    state->drivers.push_back({playback_endpoint_name(endpoint, state->language), L"wasapi:" + endpoint.id});
  SendMessageW(state->backend_combo, CB_RESETCONTENT, 0, 0);
  int selected_index = -1;
  for (std::size_t index = 0; index < state->drivers.size(); ++index) {
    SendMessageW(
        state->backend_combo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(state->drivers[index].name.c_str()));
    if (equals_ignore_case(
            state->drivers[index].clsid, selected_clsid)) {
      selected_index = static_cast<int>(index);
    }
  }
  if (selected_index >= 0) {
    SendMessageW(state->backend_combo, CB_SETCURSEL, selected_index, 0);
  }
}

std::wstring read_saved_endpoint() {
  return soundfork::control::RelaySettings{}.endpoint();
}

bool save_endpoint(AppState* state, std::wstring_view endpoint_id) {
  const DWORD code = soundfork::control::RelaySettings{}.save_endpoint(endpoint_id);
  if (code == ERROR_SUCCESS) return true;
  const auto message = std::wstring(startup_strings(state->language).save_failed) + std::to_wstring(code);
  MessageBoxW(state->window, message.c_str(), L"SoundFork", MB_ICONERROR);
  return false;
}

void populate_endpoint_combo(AppState* state) {
  const UiStrings& strings = ui_strings(state->language);
  const std::wstring saved = read_saved_endpoint();
  state->endpoints = enumerate_endpoints();
  SendMessageW(state->endpoint_combo, CB_RESETCONTENT, 0, 0);
  int selected_index = -1;
  int vb_cable_index = -1;
  int voicemeeter_desktop_index = -1;
  int voicemeeter_index = -1;
  bool vb_cable_detected = false;
  for (std::size_t index = 0; index < state->endpoints.size(); ++index) {
    std::wstring label = state->endpoints[index].name;
    if (state->endpoints[index].kind == EndpointKind::vb_cable) {
      label += std::wstring(L"  [") + strings.recommended + L"]";
    } else if (state->endpoints[index].kind == EndpointKind::voicemeeter) {
      label += std::wstring(L"  [") + strings.supported + L"]";
    }
    SendMessageW(
        state->endpoint_combo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(label.c_str()));
    if (equals_ignore_case(state->endpoints[index].id, saved) &&
        is_broadcast_endpoint(state->endpoints[index])) {
      selected_index = static_cast<int>(index);
    }
    const std::wstring lower = lowercase(state->endpoints[index].name);
    if (state->endpoints[index].kind == EndpointKind::vb_cable) {
      vb_cable_detected = true;
      if (vb_cable_index < 0) {
        vb_cable_index = static_cast<int>(index);
      }
    } else if (state->endpoints[index].kind == EndpointKind::voicemeeter) {
      if (voicemeeter_index < 0) {
        voicemeeter_index = static_cast<int>(index);
      }
      if (lower.find(L"voicemeeterdesktop") != std::wstring::npos ||
          lower.find(L"voicemeeter input") != std::wstring::npos) {
        voicemeeter_desktop_index = static_cast<int>(index);
      }
    }
  }
  if (selected_index < 0 && saved.empty()) {
    selected_index = vb_cable_index >= 0
                         ? vb_cable_index
                         : (voicemeeter_desktop_index >= 0
                                ? voicemeeter_desktop_index
                                : voicemeeter_index);
  }
  if (selected_index >= 0) {
    SendMessageW(state->endpoint_combo, CB_SETCURSEL, selected_index, 0);
  }
  if (state->vb_cable_button != nullptr) {
    SetWindowTextW(
        state->vb_cable_button,
        vb_cable_detected ? strings.vb_cable_detected : strings.get_vb_cable);
    EnableWindow(state->vb_cable_button, vb_cable_detected ? FALSE : TRUE);
  }
}

void notify_startup_error(AppState* state) {
  diagnostics::log().write(diagnostics::Level::error, "automatic_forwarding_failed");
  NOTIFYICONDATAW data{sizeof(data)};
  data.hWnd = state->window;
  data.uID = kTrayIconId;
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = NIIF_WARNING;
  wcscpy_s(data.szInfoTitle, L"SoundFork");
  const auto message = std::wstring(startup_strings(state->language).failed) + state->startup_error;
  wcsncpy_s(data.szInfo, message.c_str(), _TRUNCATE);
  (void)Shell_NotifyIconW(NIM_MODIFY, &data);
}

void start_saved_relay(AppState* state) {
  if (soundfork::control::RelaySettings{}.mode() !=
      soundfork::control::RelayMode::entire_screen) return;
  if (!soundfork::control::RelaySettings{}.startup_enabled(executable_path())) return;
  if (relay_is_running()) return;
  const auto& text = startup_strings(state->language);
  const auto saved = read_saved_endpoint();
  const auto endpoint = std::find_if(state->endpoints.begin(), state->endpoints.end(),
      [&](const Endpoint& item) { return equals_ignore_case(item.id, saved); });
  if (saved.empty() || (endpoint != state->endpoints.end() && !is_broadcast_endpoint(*endpoint))) {
    state->startup_error = text.missing;
  } else if (capture_consumer_is_active()) {
    state->startup_error = ui_strings(state->language).another_consumer;
  } else {
    // No default-device fallback. Persistent relay already waits for the game
    // and reconnects only to the exact saved endpoint, including late logon devices.
    Endpoint target{L"", saved, EndpointKind::other};
    (void)start_relay(target, state->language, &state->startup_error, &state->startup_process);
  }
  if (!state->startup_error.empty()) notify_startup_error(state);
}

bool asio_stream_is_active() {
  soundfork::capture::SharedRingObserver observer;
  soundfork::capture::StreamStatus stream;
  return observer.open() && observer.snapshot(&stream) &&
         stream.producer_active;
}

std::wstring tray_tooltip(const AppState* state) {
  const UiStrings& strings = ui_strings(state->language);
  std::wstring text = L"SoundFork - ";
  text += strings.discord_relay;
  text += L": ";
  text += relay_is_running() ? strings.running : strings.stopped;
  return text;
}

bool add_tray_icon(AppState* state) {
  NOTIFYICONDATAW data{sizeof(data)};
  data.hWnd = state->window;
  data.uID = kTrayIconId;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data.uCallbackMessage = kTrayCallbackMessage;
  data.hIcon = state->tray_icon;
  const std::wstring tooltip = tray_tooltip(state);
  _snwprintf_s(
      data.szTip, _countof(data.szTip), _TRUNCATE, L"%s", tooltip.c_str());
  if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
    state->tray_icon_added = false;
    return false;
  }
  data.uVersion = NOTIFYICON_VERSION_4;
  (void)Shell_NotifyIconW(NIM_SETVERSION, &data);
  state->tray_icon_added = true;
  return true;
}

void update_tray_icon(AppState* state) {
  if (!state->tray_icon_added) {
    return;
  }
  NOTIFYICONDATAW data{sizeof(data)};
  data.hWnd = state->window;
  data.uID = kTrayIconId;
  data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data.hIcon = state->tray_icon;
  const std::wstring tooltip = tray_tooltip(state);
  _snwprintf_s(
      data.szTip, _countof(data.szTip), _TRUNCATE, L"%s", tooltip.c_str());
  (void)Shell_NotifyIconW(NIM_MODIFY, &data);
}

void remove_tray_icon(AppState* state) {
  if (!state->tray_icon_added) {
    return;
  }
  NOTIFYICONDATAW data{sizeof(data)};
  data.hWnd = state->window;
  data.uID = kTrayIconId;
  (void)Shell_NotifyIconW(NIM_DELETE, &data);
  state->tray_icon_added = false;
}

void show_tray_notification(AppState* state) {
  if (!state->tray_icon_added || state->tray_notification_shown) {
    return;
  }
  const UiStrings& strings = ui_strings(state->language);
  NOTIFYICONDATAW data{sizeof(data)};
  data.hWnd = state->window;
  data.uID = kTrayIconId;
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = NIIF_INFO;
  _snwprintf_s(
      data.szInfoTitle,
      _countof(data.szInfoTitle),
      _TRUNCATE,
      L"%s",
      strings.tray_hidden_title);
  _snwprintf_s(
      data.szInfo,
      _countof(data.szInfo),
      _TRUNCATE,
      L"%s",
      strings.tray_hidden_message);
  if (Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE) {
    state->tray_notification_shown = true;
  }
}

void show_control_window(AppState* state) {
  ShowWindow(state->window, IsIconic(state->window) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(state->window);
}

void show_tray_menu(AppState* state) {
  const UiStrings& strings = ui_strings(state->language);
  const bool relay_running = relay_is_running();
  const bool asio_active = asio_stream_is_active();
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }

  std::wstring status = strings.asio_stream;
  status += L": ";
  status += asio_active ? strings.active : strings.inactive;
  status += L"  ·  ";
  status += strings.discord_relay;
  status += L": ";
  status += relay_running ? strings.running : strings.stopped;

  AppendMenuW(menu, MF_STRING, kTrayOpen, strings.tray_open);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(
      menu,
      MF_STRING,
      kTrayToggleRelay,
      relay_running ? strings.stop_relay : strings.start_relay);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(
      menu, MF_STRING, kTrayExitControl, strings.tray_exit_control);
  AppendMenuW(
      menu,
      MF_STRING | (relay_running ? 0 : MF_GRAYED),
      kTrayStopAndExit,
      strings.tray_stop_and_exit);
  SetMenuDefaultItem(menu, kTrayOpen, FALSE);

  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(state->window);
  const UINT command = TrackPopupMenu(
      menu,
      TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
      cursor.x,
      cursor.y,
      0,
      state->window,
      nullptr);
  DestroyMenu(menu);
  PostMessageW(state->window, WM_NULL, 0, 0);
  if (command != 0) {
    PostMessageW(state->window, WM_COMMAND, command, 0);
  }
}

void update_panel_status(AppState* state, const RegistrationStatus& registration,
    const soundfork::capture::StreamStatus& stream, bool active);

void update_status(AppState* state) {
  const UiStrings& strings = ui_strings(state->language);
  const RegistrationStatus registration = query_registration();
  const std::wstring backend =
      find_driver_name(state->drivers, registration.backend_clsid);
  const bool ready = registration.asio_registered &&
                     registration.com_registered && registration.dll_exists &&
                     !backend.empty();

  std::wostringstream text;
  text << strings.asio_driver << L": "
       << (ready ? strings.ready : strings.not_ready) << L"\r\n";
  text << strings.backend << L": "
       << (backend.empty() ? strings.not_configured : backend) << L"\r\n";
  text << strings.driver_dll << L": "
       << (registration.driver_dll.empty() ? strings.not_registered
                                           : registration.driver_dll)
       << L"\r\n\r\n";

  soundfork::capture::SharedRingObserver observer;
  soundfork::capture::StreamStatus stream;
  const bool stream_observed = observer.open() && observer.snapshot(&stream);
  const auto channel_pair = soundfork::capture::load_channels();
  text << (state->language == UiLanguage::korean ? L"방송 채널 선택 (재시작 후 적용): 왼쪽 "
                                                : L"Capture selection (apply on restart): left ")
       << channel_pair.left + 1 << L", "
       << (state->language == UiLanguage::korean ? L"오른쪽 " : L"right ") << channel_pair.right + 1 << L"\r\n";
  if (stream_observed && stream.capture_error != soundfork::capture::CaptureError::none) {
    text << strings.format << L": " << soundfork::capture::capture_error_text(
        stream.capture_error, state->language == UiLanguage::korean) << L"\r\n";
  } else if (stream_observed && stream.producer_active) {
    text << strings.asio_stream << L": " << strings.active << L" (PID "
         << stream.producer_pid << L")\r\n";
    text << strings.format << L": " << stream.format.sample_rate << L" Hz, "
         << stream.format.channel_count << L" " << strings.channels << L", "
         << stream.format.frames_per_block << L" " << strings.frames_per_block
         << L", " << soundfork::capture::sample_format_name(stream.format.sample_format) << L" (planar)\r\n";
    text << strings.published_consumed << L": " << stream.stats.published_blocks
         << L" / " << stream.stats.consumed_blocks << L"\r\n";
    text << strings.dropped_blocks << L": " << stream.stats.dropped_blocks
         << L"\r\n";
    text << strings.capture_consumer << L": "
         << (stream.consumer_active
                 ? std::wstring(strings.active) + L" (PID " +
                       std::to_wstring(stream.consumer_pid) + L")"
                 : strings.none)
         << L"\r\n";
  } else {
    text << strings.asio_stream << L": " << strings.inactive << L"\r\n";
    HANDLE legacy = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\SoundFork.Audio.v2");
    if (legacy != nullptr) {
      CloseHandle(legacy);
      text << (state->language == UiLanguage::korean
          ? L"구버전 복제 스트림: 드라이버·허브·전달·OBS 플러그인을 같은 빌드로 업데이트하세요.\r\n"
          : L"Legacy capture stream: update driver, hub, relay and OBS plugin to the same build.\r\n");
    }
  }

  const auto& obs_text = soundfork::control::obs_plugin_strings(state->language == UiLanguage::korean);
  text << obs_text.menu << L": " << obs_installed_text(state) << L"\r\n";

  const bool relay_running = relay_is_running();
  const auto relay_mode = soundfork::control::RelaySettings{}.mode();
  const LRESULT selected =
      SendMessageW(state->endpoint_combo, CB_GETCURSEL, 0, 0);
  const Endpoint* broadcast_endpoint =
      selected >= 0 &&
              static_cast<std::size_t>(selected) < state->endpoints.size()
          ? &state->endpoints[static_cast<std::size_t>(selected)]
          : nullptr;
  text << L"\r\n" << strings.broadcast << L": ";
  if (broadcast_endpoint != nullptr &&
      is_broadcast_endpoint(*broadcast_endpoint)) {
    text << strings.ready << L" (" << endpoint_kind_name(broadcast_endpoint->kind)
         << L")\r\n";
  } else {
    text << strings.broadcast_not_ready << L"\r\n";
  }
  text << strings.discord_relay << L": "
       << (relay_running ? strings.running : strings.stopped) << L"\r\n";
  text << relay_mode_strings(state->language).label << L": "
       << (relay_mode == soundfork::control::RelayMode::application
               ? relay_mode_strings(state->language).application
               : relay_mode_strings(state->language).entire_screen)
       << L"\r\n";
  const auto& startup = startup_strings(state->language);
  const bool startup_preferred =
      soundfork::control::RelaySettings{}.startup_preferred(executable_path());
  text << (startup_preferred
               ? (relay_mode == soundfork::control::RelayMode::application
                      ? startup.preserved : startup.enabled)
               : startup.disabled)
       << L"\r\n";
  if (!state->startup_error.empty()) text << startup.failed << state->startup_error << L"\r\n";
  const auto saved = read_saved_endpoint();
  if (!saved.empty() && std::none_of(state->endpoints.begin(), state->endpoints.end(),
      [&](const Endpoint& item) { return equals_ignore_case(item.id, saved); }))
    text << startup.missing << L"\r\n";
  if (relay_running && (broadcast_endpoint == nullptr || !stream.producer_active))
    text << startup.waiting << L"\r\n";
  text << strings.discord_mode << L"\r\n";
  text << strings.application_share_note;
  const auto logs = diagnostics::log().stats();
  text << L"\r\n\r\n" << (state->language == UiLanguage::korean ? L"진단 로그: " : L"Diagnostic logs: ")
       << diagnostics::log_directory().native() << L"\r\n"
       << L"logger_dropped=" << logs.dropped << L", logger_io_errors=" << logs.io_errors;
  state->diagnostic_snapshot = L"SoundFork v" SOUNDFORK_VERSION_WIDE L"\r\n\r\n" + text.str();
  if (state->diagnostic_text != nullptr) {
    // Keep reading/selection position while the existing UI timer refreshes data.
    DWORD selection_start = 0, selection_end = 0;
    SendMessageW(state->diagnostic_text, EM_GETSEL,
        reinterpret_cast<WPARAM>(&selection_start), reinterpret_cast<LPARAM>(&selection_end));
    const auto first_line = SendMessageW(state->diagnostic_text, EM_GETFIRSTVISIBLELINE, 0, 0);
    SetWindowTextW(state->diagnostic_text, state->diagnostic_snapshot.c_str());
    SendMessageW(state->diagnostic_text, EM_SETSEL, selection_start, selection_end);
    const auto new_first_line = SendMessageW(state->diagnostic_text, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(state->diagnostic_text, EM_LINESCROLL, 0, first_line - new_first_line);
  }
  const soundfork::control::ControlStatus summary{
      ready, relay_running,
      broadcast_endpoint != nullptr && is_broadcast_endpoint(*broadcast_endpoint),
      stream.producer_active, state->startup_error};
  const int observed = (relay_running ? 1 : 0) | (stream.producer_active ? 2 : 0) |
      (summary.device_available ? 4 : 0);
  if (observed != state->logged_status) {
    state->logged_status = observed;
    diagnostics::log().write(diagnostics::Level::info, "observed_state",
        "relay=" + std::to_string(relay_running) + " asio_stream=" +
        std::to_string(stream.producer_active) + " device_available=" + std::to_string(summary.device_available));
  }
  const bool korean = state->language == UiLanguage::korean;
  std::wstring footer = ready ? (korean ? L"재생 설정 준비됨" : L"Playback configured")
      : (korean ? L"재생 설정 확인 필요" : L"Check playback settings");
  footer += relay_running ? (korean ? L" · 오디오 전달 실행 중" : L" · Forwarding running")
      : (korean ? L" · 오디오 전달 중지됨" : L" · Forwarding stopped");
  if (!state->startup_error.empty()) footer = korean
      ? L"자동 전달 시작 실패 · 연결 정보에서 진단을 확인하세요."
      : L"Automatic forwarding failed · See Connection diagnostics.";
  SetWindowTextW(state->status, footer.c_str());
  update_panel_status(state, registration, stream, stream_observed && stream.producer_active);
  SendMessageW(state->startup_checkbox, BM_SETCHECK,
      startup_preferred ? BST_CHECKED : BST_UNCHECKED, 0);
  SetWindowTextW(
      state->relay_button,
      relay_running ? strings.stop_relay
                    : (relay_mode == soundfork::control::RelayMode::application
                           ? relay_mode_strings(state->language).starts_with_game
                           : strings.start_relay));
  EnableWindow(state->relay_button,
      (soundfork::capture::relay_enabled() &&
       relay_mode == soundfork::control::RelayMode::entire_screen) || relay_running);
  SendMessageW(state->relay_enabled, BM_SETCHECK,
      soundfork::capture::relay_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
  if (stream.format.frames_per_block != 0 && stream.format.sample_rate != 0 &&
      !SendMessageW(state->relay_buffer, CB_GETDROPPEDSTATE, 0, 0)) {
    const auto buffer_selected = SendMessageW(state->relay_buffer, CB_GETCURSEL, 0, 0);
    for (int index = 0; index < static_cast<int>(std::size(kRelayBuffers)); ++index) {
      const auto frames = soundfork::capture::relay_target_frames(
          stream.format.frames_per_block, kRelayBuffers[index]);
      std::wostringstream label;
      if (index == 0) label << (state->language == UiLanguage::korean ? L"자동 — " : L"Auto — ");
      label << frames << L" frames (" << std::fixed << std::setprecision(2)
            << static_cast<double>(frames) * 1000.0 / stream.format.sample_rate << L" ms)";
      SendMessageW(state->relay_buffer, CB_DELETESTRING, index, 0);
      SendMessageW(state->relay_buffer, CB_INSERTSTRING, index,
          reinterpret_cast<LPARAM>(label.str().c_str()));
    }
    SendMessageW(state->relay_buffer, CB_SETCURSEL, buffer_selected, 0);
  }
  EnableWindow(state->startup_checkbox,
      relay_mode == soundfork::control::RelayMode::entire_screen);
  SetWindowTextW(state->note,
      registration.backend_clsid.starts_with(L"wasapi:")
          ? (state->language == UiLanguage::korean
              ? L"Windows 출력은 앱의 오디오로 재생됩니다. 중복 캡처를 피하려면 릴레이 전달은 끄고 OBS는 전용 소스를 사용하세요."
              : L"Windows output belongs to the app. Disable forwarding to avoid duplicate capture; use the dedicated OBS source.")
          : relay_mode == soundfork::control::RelayMode::application
          ? relay_mode_strings(state->language).application_note
          : strings.note);
  EnableWindow(state->endpoint_combo, relay_running ? FALSE : TRUE);
  update_tray_icon(state);
}

bool copy_diagnostics(AppState* state) {
  auto text = state->diagnostic_snapshot;
  // Preserve the on-screen diagnostic detail, but never copy local file paths.
  for (const auto& path : {query_registration().driver_dll,
                           diagnostics::log_directory().native()}) {
    if (path.empty()) continue;
    std::size_t position = 0;
    while ((position = text.find(path, position)) != std::wstring::npos) {
      text.replace(position, path.size(), L"[local path omitted]");
      position += 20;
    }
  }
  const auto bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) return false;
  auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
  if (destination == nullptr) { GlobalFree(memory); return false; }
  std::copy_n(text.c_str(), text.size() + 1, destination);
  GlobalUnlock(memory);
  bool copied = false;
  if (OpenClipboard(state->diagnostic_window != nullptr ? state->diagnostic_window : state->window)) {
    if (EmptyClipboard()) copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
  }
  if (!copied) GlobalFree(memory);  // Windows owns it only after successful SetClipboardData.
  return copied;
}

LRESULT CALLBACK diagnostic_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    state = static_cast<AppState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
  const auto& text = soundfork::control::diagnostic_strings(state->language == UiLanguage::korean);
  switch (message) {
    case WM_CREATE:
      state->diagnostic_window = window;
      state->diagnostic_text = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
          12, 12, 600, 300, window, nullptr, nullptr, nullptr);
      state->diagnostic_copy = CreateWindowExW(0, L"BUTTON", text.copy,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
          12, 320, 110, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyDiagnostics)), nullptr, nullptr);
      state->diagnostic_close = CreateWindowExW(0, L"BUTTON", text.close,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          510, 320, 110, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseDiagnostics)), nullptr, nullptr);
      if (!state->diagnostic_text || !state->diagnostic_copy || !state->diagnostic_close) return -1;
      for (HWND item : {state->diagnostic_text, state->diagnostic_copy, state->diagnostic_close})
        set_control_font(item, state->font);
      update_status(state);
      return 0;
    case WM_GETMINMAXINFO: {
      auto* size = reinterpret_cast<MINMAXINFO*>(lparam);
      size->ptMinTrackSize = {540, 320};
      return 0;
    }
    case WM_SIZE: {
      const int width = LOWORD(lparam), height = HIWORD(lparam);
      if (state->diagnostic_text) {
        MoveWindow(state->diagnostic_text, 12, 12, (std::max)(100, width - 24), (std::max)(80, height - 66), TRUE);
        MoveWindow(state->diagnostic_copy, 12, (std::max)(100, height - 42), 110, 30, TRUE);
        MoveWindow(state->diagnostic_close, (std::max)(140, width - 122), (std::max)(100, height - 42), 110, 30, TRUE);
      }
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kCopyDiagnostics && HIWORD(wparam) == BN_CLICKED) {
        if (!copy_diagnostics(state)) MessageBoxW(window, text.copy_failed, L"SoundFork", MB_ICONERROR);
        return 0;
      }
      if (LOWORD(wparam) == kCloseDiagnostics || LOWORD(wparam) == IDCANCEL) {
        DestroyWindow(window);
        return 0;
      }
      break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
      state->diagnostic_window = nullptr;
      state->diagnostic_text = nullptr;
      state->diagnostic_copy = nullptr;
      state->diagnostic_close = nullptr;
      return 0;
    default: break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void show_diagnostics(AppState* state) {
  if (state->diagnostic_window != nullptr) {
    ShowWindow(state->diagnostic_window, SW_RESTORE);
    SetForegroundWindow(state->diagnostic_window);
    return;
  }
  const auto& text = soundfork::control::diagnostic_strings(state->language == UiLanguage::korean);
  const auto title = std::wstring(L"SoundFork — ") + text.title;
  HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, kDiagnosticClass, title.c_str(),
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 740, 480,
                                state->window, nullptr, GetModuleHandleW(nullptr), state);
  if (window == nullptr) {
    MessageBoxW(state->window, text.open_failed, L"SoundFork", MB_ICONERROR);
    return;
  }
  ShowWindow(window, SW_SHOWNORMAL);
  SetFocus(state->diagnostic_text);
}

LRESULT control_background(WPARAM wparam, LPARAM lparam) {
  const auto dc = reinterpret_cast<HDC>(wparam);
  SetBkColor(dc, GetSysColor(COLOR_WINDOW));
  SetTextColor(dc, GetSysColor(IsWindowEnabled(reinterpret_cast<HWND>(lparam)) ? COLOR_WINDOWTEXT
                                                                               : COLOR_GRAYTEXT));
  return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
}
struct ChannelDialog {
  HWND left = nullptr, right = nullptr;
  soundfork::capture::ChannelPair pair{};
  bool accepted = false, korean = true;
  HFONT font = nullptr;
};
LRESULT CALLBACK channel_dialog(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<ChannelDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    state = static_cast<ChannelDialog*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (!state) return DefWindowProcW(window, message, wparam, lparam);
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN)
    return control_background(wparam, lparam);
  if (message == WM_CREATE) {
    for (int side = 0; side < 2; ++side) {
      HWND label = CreateWindowW(L"STATIC",
                                 side == 0 ? (state->korean ? L"왼쪽 출력" : L"Left output")
                                           : (state->korean ? L"오른쪽 출력" : L"Right output"),
                                 WS_CHILD | WS_VISIBLE, 24, 24 + side * 44, 110, 24, window,
                                 nullptr, nullptr, nullptr);
      HWND combo = CreateWindowW(L"COMBOBOX", nullptr,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 146, 20 + side * 44, 220, 240, window, nullptr, nullptr, nullptr);
      for (unsigned index = 0; index < 64; ++index) {
        const auto text =
            std::wstring(state->korean ? L"출력 " : L"Output ") + std::to_wstring(index + 1);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
      }
      SendMessageW(combo, CB_SETCURSEL, side == 0 ? state->pair.left : state->pair.right, 0);
      if (side == 0)
        state->left = combo;
      else
        state->right = combo;
      set_control_font(label, state->font);
      set_control_font(combo, state->font);
    }
    HWND note = CreateWindowW(
        L"STATIC",
        state->korean ? L"게임·DAW 재시작 후 적용됩니다." : L"Restart the game or DAW to apply.",
        WS_CHILD | WS_VISIBLE, 24, 110, 350, 24, window, nullptr, nullptr, nullptr);
    HWND ok = CreateWindowW(L"BUTTON", state->korean ? L"확인" : L"OK",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 164, 150, 94, 30,
                            window, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
    HWND cancel = CreateWindowW(L"BUTTON", state->korean ? L"취소" : L"Cancel",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 270, 150, 94, 30, window,
                                reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
    for (auto item : {note, ok, cancel}) set_control_font(item, state->font);
    SetFocus(state->left);
    return 0;
  }
  if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
    const auto left = SendMessageW(state->left, CB_GETCURSEL, 0, 0);
    const auto right = SendMessageW(state->right, CB_GETCURSEL, 0, 0);
    if (left >= 0 && right >= 0 && left < 64 && right < 64) {
      state->pair = {static_cast<DWORD>(left), static_cast<DWORD>(right)};
      state->accepted = true;
      DestroyWindow(window);
    }
    return 0;
  }
  if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
    DestroyWindow(window);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void layout_controls(AppState* state, int) {
  auto& p = state->panel;
  if (!p.ready) return;
  RECT r{};
  GetClientRect(state->window, &r);
  const int width = r.right, field = width - 164;
  const auto move = [](HWND h, int x, int y, int w, int height) {
    MoveWindow(h, x, y, w, height, TRUE);
  };
  move(state->output_group, 12, 12, width - 24, 82);
  move(state->backend_combo, 26, 42, width - 146, 200);
  move(state->apply_button, width - 108, 41, 82, 26);
  move(p.playback_info, 12, 108, width - 24, 110);
  move(p.playback_note, 26, 132, width - 52, 72);
  move(state->refresh_button, 26, 234, 130, 26);
  move(state->forwarding_group, 12, 12, width - 24, 238);
  move(state->relay_enabled, 26, 36, width - 200, 24);
  move(state->relay_button, width - 178, 34, 152, 26);
  move(state->relay_mode_label, 26, 84, 112, 20);
  move(state->relay_mode_combo, 138, 80, field, 160);
  move(state->endpoint_label, 26, 126, 112, 20);
  move(state->endpoint_combo, 138, 122, field, 200);
  move(state->startup_checkbox, 26, 166, width - 52, 24);
  move(state->vb_cable_button, 26, 208, 158, 24);
  move(state->note, 26, 266, width - 52, 44);
  move(p.buffer_group, 12, 12, width - 24, 142);
  move(state->buffer_label, 26, 48, 112, 20);
  move(state->relay_buffer, 138, 44, field, 200);
  move(p.channel_label, 26, 100, 112, 20);
  move(p.channels, 138, 96, field - 86, 200);
  move(p.custom, width - 100, 95, 74, 26);
  move(p.buffer_note, 26, 176, width - 52, 100);
  move(p.obs_title, 12, 12, width - 24, 104);
  move(p.obs_version_label, 26, 42, 112, 20);
  move(p.obs_status, 138, 42, field, 24);
  move(p.obs_bundle_label, 26, 78, 112, 20);
  move(p.obs_bundle, 138, 78, field, 20);
  move(p.obs_actions, 12, 130, width - 24, 70);
  move(p.obs_install, 26, 158, 180, 26);
  move(p.obs_remove, 218, 158, 90, 26);
  move(p.obs_details, 320, 158, width - 346, 26);
  move(p.obs_guide, 12, 214, width - 24, 96);
  move(p.obs_usage, 26, 238, width - 52, 58);
  move(p.settings_title, 12, 12, width - 24, 86);
  move(p.language_label, 26, 48, 112, 20);
  move(p.language, 138, 44, 210, 120);
  move(p.update_group, 12, 112, width - 24, 100);
  move(p.updates, 26, 140, width - 52, 24);
  move(p.settings_note, 26, 174, width - 52, 22);
  move(p.information_group, 12, 12, width - 24, 198);
  move(p.information, 26, 36, width - 52, 158);
  SendMessageW(p.information, LVM_SETCOLUMNWIDTH, 0, 150);
  SendMessageW(p.information, LVM_SETCOLUMNWIDTH, 1, width - 206);
  move(p.diagnostic_button, 26, 226, 150, 26);
  move(p.logs_button, 188, 226, 150, 26);
  move(p.program_group, 12, 12, width - 24, 150);
  move(p.program_name, 26, 36, width - 52, 30);
  move(p.program, 26, 80, width - 52, 64);
  move(p.support_group, 12, 176, width - 24, 70);
  move(p.update_button, 26, 204, 150, 26);
  move(p.bug_button, 188, 204, 138, 26);
  move(p.project_button, 338, 204, width - 364, 26);
  move(state->status, 14, r.bottom - 25, width - 28, 18);
  for (int page = 0; page < kPageCount; ++page)
    for (auto control : p.pages[page]) ShowWindow(control, p.page == page ? SW_SHOW : SW_HIDE);
  if (const auto focus = GetFocus();
      focus && IsChild(state->window, focus) && !IsWindowVisible(focus))
    SetFocus(GetNextDlgTabItem(state->window, nullptr, FALSE));
  InvalidateRect(state->window, nullptr, TRUE);
}

HWND panel_widget(AppState* state, const wchar_t* kind, const wchar_t* text, int id,
                  DWORD style = 0) {
  HWND control =
      CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, state->window,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
  set_control_font(control, state->font);
  return control;
}

void populate_channels(AppState* state) {
  auto& p = state->panel;
  if (SendMessageW(p.channels, CB_GETDROPPEDSTATE, 0, 0)) return;
  SendMessageW(p.channels, CB_RESETCONTENT, 0, 0);
  const bool ko = state->language == UiLanguage::korean;
  for (unsigned index = 0; index < 32; ++index) {
    const auto text = std::wstring(ko ? L"출력 " : L"Outputs ") + std::to_wstring(index * 2 + 1) +
                      L" · " + std::to_wstring(index * 2 + 2);
    SendMessageW(p.channels, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
  }
  const auto custom = std::wstring(ko ? L"사용자 지정 — " : L"Custom — ") +
                      std::to_wstring(p.pair.left + 1) + L" / " + std::to_wstring(p.pair.right + 1);
  SendMessageW(p.channels, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(custom.c_str()));
  SendMessageW(p.channels, CB_SETCURSEL,
               p.pair.left % 2 == 0 && p.pair.right == p.pair.left + 1 ? p.pair.left / 2 : 32, 0);
}
void translate_panel(AppState* state) {
  auto& p = state->panel;
  const bool ko = state->language == UiLanguage::korean;
  SetWindowTextW(p.channel_label, ko ? L"방송 채널" : L"Channels");
  SetWindowTextW(state->relay_mode_label, ko ? L"오디오 전달 방식" : L"Mode");
  SetWindowTextW(state->endpoint_label, ko ? L"전달 장치" : L"Device");
  SetWindowTextW(state->buffer_label, ko ? L"전달 버퍼" : L"Buffer");
  SetWindowTextW(p.custom, ko ? L"변경…" : L"Edit…");
  SetWindowTextW(p.playback_info, ko ? L"ASIO 연결 상태" : L"ASIO stream status");
  SetWindowTextW(state->output_group, ko ? L"재생 출력" : L"Playback output");
  SetWindowTextW(p.buffer_group, ko ? L"방송 오디오 설정" : L"Broadcast audio settings");
  SetWindowTextW(p.buffer_note,
                 ko ? L"버퍼 시간은 방송용 여유량이며 실측 지연이 아닙니다.\r\n\r\n채널 변경은 "
                      L"게임·DAW 재시작 후 적용됩니다."
                    : L"Buffer duration is broadcast headroom, not measured "
                      L"latency.\r\n\r\nRestart the game or DAW to apply channel changes.");
  SetWindowTextW(p.obs_title, ko ? L"플러그인 상태" : L"Plugin status");
  SetWindowTextW(p.obs_version_label, ko ? L"설치된 버전" : L"Installed version");
  SetWindowTextW(p.obs_bundle_label, ko ? L"포함된 버전" : L"Bundled version");
  SetWindowTextW(p.obs_actions, ko ? L"플러그인 관리" : L"Plugin management");
  SetWindowTextW(p.obs_guide, ko ? L"사용 방법" : L"Getting started");
  SetWindowTextW(p.obs_install, ko ? L"설치 / 업데이트…" : L"Install / update…");
  SetWindowTextW(p.obs_remove, ko ? L"제거…" : L"Remove…");
  SetWindowTextW(p.obs_details, ko ? L"자세히…" : L"Details…");
  SetWindowTextW(p.obs_usage, ko ? L"OBS 재시작 → ‘SoundFork 오디오’ 소스 추가\r\nOBS만 사용하면 "
                                   L"가상 장치나 오디오 전달은 필요 없습니다."
                                 : L"1. Restart OBS.\r\n2. Add a SoundFork Audio "
                                   L"source.\r\nOBS-only capture needs no virtual audio device.");
  SetWindowTextW(p.settings_title, ko ? L"표시" : L"Appearance");
  SetWindowTextW(p.language_label, ko ? L"표시 언어" : L"Language");
  SendMessageW(p.language, CB_SETCURSEL, ko ? 0 : 1, 0);
  SetWindowTextW(p.updates, ko ? L"새 버전 자동 알림" : L"Notify me about new versions");
  SetWindowTextW(p.update_group, ko ? L"업데이트" : L"Updates");
  SetWindowTextW(p.settings_note, ko ? L"직접 확인하려면 ‘프로그램 정보’를 여세요."
                                     : L"Check manually under About.");
  SetWindowTextW(p.diagnostic_button, ko ? L"상세 진단 보기…" : L"Detailed diagnostics…");
  SetWindowTextW(p.logs_button, ko ? L"로그 폴더 열기…" : L"Open log folder…");
  SetWindowTextW(p.program_group, ko ? L"프로그램 정보" : L"About SoundFork");
  SetWindowTextW(p.program_name, L"SoundFork");
  SetWindowTextW(p.program,ko?L"버전  " SOUNDFORK_VERSION_WIDE L"\r\nWindows x64 · MIT 라이선스\r\nASIO 재생과 방송용 오디오 복제":
      L"Version  " SOUNDFORK_VERSION_WIDE L"\r\nWindows x64 · MIT license\r\nASIO playback and broadcast audio capture");
  SetWindowTextW(p.support_group, ko ? L"지원 및 업데이트" : L"Support and updates");
  SetWindowTextW(p.update_button, ko ? L"업데이트 확인…" : L"Check for updates…");
  SetWindowTextW(p.bug_button, ko ? L"버그 제보…" : L"Report a bug…");
  SetWindowTextW(p.project_button, ko ? L"프로젝트…" : L"Project…");
  populate_channels(state);
  update_obs_controls(state);
  SendMessageW(p.updates, BM_SETCHECK,
               state->automatic_updates_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  EnableWindow(p.update_button, !state->update_check_in_progress);
}

void update_panel_status(AppState* state, const RegistrationStatus& registration,
                         const soundfork::capture::StreamStatus& stream, bool active) {
  auto& p = state->panel;
  if (!p.ready) return;
  const bool ko = state->language == UiLanguage::korean;
  const auto pair = soundfork::capture::load_channels();
  if (pair.left != p.pair.left || pair.right != p.pair.right) {
    p.pair = pair;
    populate_channels(state);
  }
  std::wstring stream_text = ko ? (active ? L"상태: 연결됨" : L"상태: 연결 대기")
                                : (active ? L"Status: connected" : L"Status: waiting");
  if (active) {
    stream_text += L"\r\n" + std::to_wstring(stream.format.sample_rate) + L" Hz · " +
                   std::to_wstring(stream.format.frames_per_block) + L" frames";
    std::wostringstream format;
    format << soundfork::capture::sample_format_name(stream.format.sample_format);
    stream_text += L"\r\n" + format.str();
  } else
    stream_text += ko ? L"\r\n게임·DAW에서 SoundFork ASIO를 선택하세요."
                      : L"\r\nSelect SoundFork ASIO in your game or DAW.";
  SetWindowTextW(p.playback_note, stream_text.c_str());
  SetWindowTextW(p.information_group, ko ? L"연결 정보" : L"Connection information");
  SendMessageW(p.information, LVM_DELETEALLITEMS, 0, 0);
  const auto add_property = [&](const wchar_t* label, const std::wstring& value) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(SendMessageW(p.information, LVM_GETITEMCOUNT, 0, 0));
    item.pszText = const_cast<wchar_t*>(label);
    const auto index =
        SendMessageW(p.information, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    item.iItem = static_cast<int>(index);
    item.iSubItem = 1;
    item.pszText = const_cast<wchar_t*>(value.c_str());
    SendMessageW(p.information, LVM_SETITEMTEXTW, static_cast<WPARAM>(index),
                 reinterpret_cast<LPARAM>(&item));
  };
  add_property(ko ? L"드라이버 등록" : L"Driver registration",
               registration.asio_registered ? (ko ? L"등록됨" : L"Registered")
                                            : (ko ? L"미등록" : L"Not registered"));
  add_property(ko ? L"ASIO 연결" : L"ASIO stream",
               active ? (ko ? L"연결됨" : L"Connected") : (ko ? L"대기 중" : L"Waiting"));
  add_property(ko ? L"샘플레이트" : L"Sample rate",
               active ? std::to_wstring(stream.format.sample_rate) + L" Hz" : L"—");
  add_property(ko ? L"버퍼 크기" : L"Block size",
               active ? std::to_wstring(stream.format.frames_per_block) + L" frames" : L"—");
  std::wostringstream format_name;
  if (active) format_name << soundfork::capture::sample_format_name(stream.format.sample_format);
  add_property(ko ? L"샘플 형식" : L"Sample format", active ? format_name.str() : L"—");
  add_property(ko ? L"선택 채널" : L"Selected channels",
               std::to_wstring(pair.left + 1) + L" / " + std::to_wstring(pair.right + 1));
}

bool create_controls(AppState* state) {
  auto& p = state->panel;
  state->font =
      CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  const auto group = [&](const wchar_t* text) {
    return panel_widget(state, L"BUTTON", text, 0, BS_GROUPBOX);
  };
  const auto combo = [&](int id) {
    return panel_widget(state, L"COMBOBOX", L"", id, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
  };
  const auto label = [&]() { return panel_widget(state, L"STATIC", L"", 0); };
  const auto button = [&](int id, DWORD style = BS_PUSHBUTTON) {
    return panel_widget(state, L"BUTTON", L"", id, WS_TABSTOP | style);
  };
  state->output_group = group(L"");
  state->backend_combo = combo(kBackendCombo);
  state->apply_button = button(kApplyBackend);
  state->forwarding_group = group(L"");
  state->relay_enabled = button(kRelayEnabled, BS_AUTOCHECKBOX);
  state->relay_button = button(kToggleRelay);
  state->relay_mode_label = label();
  state->relay_mode_combo = combo(kRelayModeCombo);
  state->endpoint_label = label();
  state->endpoint_combo = combo(kEndpointCombo);
  state->buffer_label = label();
  state->relay_buffer = combo(kRelayBuffer);
  const DWORD saved_buffer = soundfork::capture::relay_preference(L"RelayBufferFrames", 0);
  for (int index = 0; index < static_cast<int>(std::size(kRelayBuffers)); ++index) {
    const auto frames = kRelayBuffers[index];
    const auto text = frames == 0 ? (state->language == UiLanguage::korean
                                         ? std::wstring(L"자동 — ASIO 연결 대기")
                                         : std::wstring(L"Auto — waiting for ASIO"))
                                  : std::to_wstring(frames) + L" frames";
    SendMessageW(state->relay_buffer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    if (saved_buffer == frames) SendMessageW(state->relay_buffer, CB_SETCURSEL, index, 0);
  }
  if (SendMessageW(state->relay_buffer, CB_GETCURSEL, 0, 0) < 0)
    SendMessageW(state->relay_buffer, CB_SETCURSEL, 0, 0);
  state->refresh_button = button(kRefresh);
  state->vb_cable_button = button(kGetVbCable);
  state->startup_checkbox = button(kStartupRelay, BS_AUTOCHECKBOX);
  state->note = label();
  state->status = panel_widget(state, L"STATIC", L"", kStatus, SS_LEFT);
  p.pair = soundfork::capture::load_channels();
  p.channel_label = panel_widget(state, L"STATIC", L"", 0);
  p.channels =
      panel_widget(state, L"COMBOBOX", L"", kChannels, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
  p.custom = panel_widget(state, L"BUTTON", L"", kCustomChannels, WS_TABSTOP);
  p.obs_title = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  p.obs_status = panel_widget(state, L"STATIC", L"", 0);
  p.obs_version_label = panel_widget(state, L"STATIC", L"", 0);
  p.obs_bundle_label = panel_widget(state, L"STATIC", L"", 0);
  p.obs_bundle = panel_widget(state, L"STATIC", L"", 0);
  p.obs_actions = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  p.obs_install = panel_widget(state, L"BUTTON", L"", kObsInstall, WS_TABSTOP);
  p.obs_remove = panel_widget(state, L"BUTTON", L"", kObsRemove, WS_TABSTOP);
  p.obs_details = panel_widget(state, L"BUTTON", L"", kObsInformation, WS_TABSTOP);
  p.obs_guide = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  p.obs_usage = panel_widget(state, L"STATIC", L"", 0);
  p.settings_title = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  p.language_label = panel_widget(state, L"STATIC", L"", 0);
  p.update_group = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  p.language =
      panel_widget(state, L"COMBOBOX", L"", kDisplayLanguage, WS_TABSTOP | CBS_DROPDOWNLIST);
  for (const auto* text : {L"한국어", L"English"})
    SendMessageW(p.language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
  p.updates = panel_widget(state, L"BUTTON", L"", kAutomaticUpdateNotifications,
                           WS_TABSTOP | BS_AUTOCHECKBOX);
  SendMessageW(p.updates, BM_SETCHECK, BST_CHECKED, 0);
  p.settings_note = panel_widget(state, L"STATIC", L"", 0);
  p.playback_note = panel_widget(state, L"STATIC", L"", 0);
  p.buffer_group = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  SetWindowPos(p.buffer_group, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  p.buffer_note = panel_widget(state, L"STATIC", L"", 0);
  p.playback_info = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  p.information_group = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  p.information =
      panel_widget(state, WC_LISTVIEWW, L"", 0, LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL);
  SendMessageW(p.information, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
               LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  for (int i = 0; i < 2; ++i) {
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = i == 0 ? 150 : 300;
    SendMessageW(p.information, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
                 reinterpret_cast<LPARAM>(&column));
  }
  p.diagnostic_button = panel_widget(state, L"BUTTON", L"", kShowDiagnostics, WS_TABSTOP);
  p.logs_button = panel_widget(state, L"BUTTON", L"", kOpenLogs, WS_TABSTOP);
  p.program_group = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  p.program = panel_widget(state, L"STATIC", L"", 0);
  p.program_name = panel_widget(state, L"STATIC", L"", 0);
  LOGFONTW font{};
  GetObjectW(state->font, sizeof(font), &font);
  font.lfHeight = -20;
  font.lfWeight = FW_SEMIBOLD;
  p.heading_font = CreateFontIndirectW(&font);
  if (p.heading_font) set_control_font(p.program_name, p.heading_font);
  p.support_group = panel_widget(state, L"BUTTON", L"", 0, BS_GROUPBOX);
  p.update_button = panel_widget(state, L"BUTTON", L"", kCheckForUpdates, WS_TABSTOP);
  p.bug_button = panel_widget(state, L"BUTTON", L"", kReportBug, WS_TABSTOP);
  SetWindowPos(p.playback_info, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  p.project_button = button(kOpenProject);
  p.pages[5] = {p.information_group, p.information, p.diagnostic_button, p.logs_button};
  p.pages[6] = {p.program_group, p.program_name, p.program,       p.support_group,
                p.update_button, p.bug_button,   p.project_button};
  p.pages[0] = {state->output_group,   state->backend_combo, state->apply_button,
                state->refresh_button, p.playback_info,      p.playback_note};
  p.pages[1] = {state->forwarding_group, state->relay_mode_label,
                state->relay_mode_combo, state->relay_enabled,
                state->relay_button,     state->endpoint_label,
                state->endpoint_combo,   state->startup_checkbox,
                state->vb_cable_button,  state->note};
  p.pages[2] = {
      p.buffer_group, p.buffer_note, state->buffer_label, state->relay_buffer, p.channel_label,
      p.channels,     p.custom};
  p.pages[3] = {p.obs_title,   p.obs_status,  p.obs_version_label, p.obs_bundle_label,
                p.obs_bundle,  p.obs_actions, p.obs_install,       p.obs_remove,
                p.obs_details, p.obs_guide,   p.obs_usage};
  p.pages[4] = {p.settings_title, p.language_label, p.language,
                p.update_group,   p.updates,        p.settings_note};
  for (const auto& page : p.pages)
    if (std::any_of(page.begin(), page.end(), [](HWND control) { return control == nullptr; }))
      return false;
  if (!state->font || !state->status) return false;
  p.ready = true;
  const auto& text = ui_strings(state->language);
  SetWindowTextW(state->apply_button, text.apply);
  SetWindowTextW(state->refresh_button, text.refresh_devices);
  SetWindowTextW(state->startup_checkbox, startup_strings(state->language).menu);
  SetWindowTextW(state->relay_enabled, state->language == UiLanguage::korean
                                           ? L"오디오 전달 사용"
                                           : L"Enable audio forwarding");
  SetWindowTextW(state->forwarding_group, state->language == UiLanguage::korean
                                              ? L"방송 오디오 전달"
                                              : L"Broadcast audio forwarding");
  SetWindowTextW(state->buffer_label,
                 state->language == UiLanguage::korean ? L"전달 버퍼" : L"Forwarding buffer");
  SetWindowTextW(state->endpoint_label,
                 state->language == UiLanguage::korean ? L"전달 장치" : L"Forwarding device");
  SetWindowTextW(state->relay_mode_label, relay_mode_strings(state->language).label);
  translate_panel(state);
  layout_controls(state, 0);
  return true;
}

bool choose_channels(AppState* state, soundfork::capture::ChannelPair* result) {
  auto& p = state->panel;
  ChannelDialog data;
  data.pair = p.pair;
  data.korean = state->language == UiLanguage::korean;
  data.font = state->font;
  RECT owner{};
  GetWindowRect(state->window, &owner);
  HWND dialog =
      CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kChannelDialogClass,
                      data.korean ? L"방송 채널 사용자 지정" : L"Custom broadcast channels",
                      WS_POPUP | WS_CAPTION | WS_SYSMENU, owner.left + 100, owner.top + 130, 412,
                      234, state->window, nullptr, GetModuleHandleW(nullptr), &data);
  if (!dialog) return false;
  EnableWindow(state->window, FALSE);
  ShowWindow(dialog, SW_SHOW);
  MSG message{};
  while (IsWindow(dialog)) {
    const auto message_result = GetMessageW(&message, nullptr, 0, 0);
    if (message_result <= 0) {
      if (message_result == 0) PostQuitMessage(static_cast<int>(message.wParam));
      break;
    }
    if (!IsDialogMessageW(dialog, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  EnableWindow(state->window, TRUE);
  SetActiveWindow(state->window);
  if (data.accepted) *result = data.pair;
  return data.accepted;
}
void save_channel_selection(AppState* state, soundfork::capture::ChannelPair pair) {
  if (soundfork::capture::save_channels(pair) == ERROR_SUCCESS) {
    state->panel.pair = pair;
    update_status(state);
  } else
    MessageBoxW(state->window,
                state->language == UiLanguage::korean ? L"채널 설정을 저장하지 못했습니다."
                                                      : L"Could not save channel settings.",
                L"SoundFork", MB_ICONERROR);
  populate_channels(state);
}

void populate_relay_mode_combo(AppState* state) {
  const auto& strings = relay_mode_strings(state->language);
  SendMessageW(state->relay_mode_combo, CB_RESETCONTENT, 0, 0);
  SendMessageW(state->relay_mode_combo, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(strings.entire_screen));
  SendMessageW(state->relay_mode_combo, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(strings.application));
  const auto mode = soundfork::control::RelaySettings{}.mode();
  SendMessageW(state->relay_mode_combo, CB_SETCURSEL,
               mode == soundfork::control::RelayMode::application ? 1 : 0, 0);
}

void rebuild_menu(AppState* state) {
  const auto previous = GetMenu(state->window);
  HMENU menu = CreateMenu();
  if (!menu) return;
  const bool ko = state->language == UiLanguage::korean;
  const wchar_t* names[] = {ko ? L"재생" : L"Playback",        ko ? L"방송" : L"Broadcast",
                            ko ? L"버퍼·채널" : L"Buffer/ch.", L"OBS",
                            ko ? L"설정" : L"Settings",        ko ? L"연결 정보" : L"Connection",
                            ko ? L"프로그램 정보" : L"About"};
  for (int page = 0; page < kPageCount; ++page)
    AppendMenuW(menu, MF_STRING, kNavigate + page, names[page]);
  SetMenuDefaultItem(menu, static_cast<UINT>(state->panel.page), TRUE);
  SetMenu(state->window, menu);
  if (previous) DestroyMenu(previous);
  DrawMenuBar(state->window);
  SendMessageW(state->panel.updates, BM_SETCHECK, state->automatic_updates_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  EnableWindow(state->panel.update_button, !state->update_check_in_progress);
}

void apply_language(AppState* state, UiLanguage language, bool persist) {
  std::wstring selected_endpoint_id;
  const LRESULT selected =
      SendMessageW(state->endpoint_combo, CB_GETCURSEL, 0, 0);
  if (selected >= 0 &&
      static_cast<std::size_t>(selected) < state->endpoints.size()) {
    selected_endpoint_id =
        state->endpoints[static_cast<std::size_t>(selected)].id;
  }
  state->language = language;
  if (persist) {
    (void)save_language(language);
  }
  const UiStrings& strings = ui_strings(language);
  SetWindowTextW(state->window, strings.window_title);
  SetWindowTextW(state->apply_button, strings.apply);
  SetWindowTextW(state->endpoint_label, language == UiLanguage::korean ? L"전달 장치" : L"Forwarding device");
  SetWindowTextW(state->relay_mode_label, relay_mode_strings(language).label);
  SetWindowTextW(state->output_group, language == UiLanguage::korean ? L"재생 출력 (ASIO 권장)" : L"Playback output (ASIO recommended)");
  SetWindowTextW(state->forwarding_group, language == UiLanguage::korean ? L"방송 오디오 전달" : L"Broadcast audio forwarding");
  SetWindowTextW(state->buffer_label, language == UiLanguage::korean ? L"전달 버퍼" : L"Forwarding buffer");
  SetWindowTextW(state->refresh_button, strings.refresh_devices);
  SetWindowTextW(state->note, strings.note);
  SetWindowTextW(state->startup_checkbox, startup_strings(language).menu);
  SetWindowTextW(state->relay_enabled, language == UiLanguage::korean
      ? L"오디오 전달 사용" : L"Enable audio forwarding");
  if (state->diagnostic_window != nullptr) {
    const auto& diagnostic = soundfork::control::diagnostic_strings(language == UiLanguage::korean);
    const auto title = std::wstring(L"SoundFork — ") + diagnostic.title;
    SetWindowTextW(state->diagnostic_window, title.c_str());
    SetWindowTextW(state->diagnostic_copy, diagnostic.copy);
    SetWindowTextW(state->diagnostic_close, diagnostic.close);
  }
  translate_panel(state);
  rebuild_menu(state);
  populate_backend_combo(state, true);
  populate_endpoint_combo(state);
  populate_relay_mode_combo(state);
  if (!selected_endpoint_id.empty()) {
    const auto endpoint = std::find_if(
        state->endpoints.begin(),
        state->endpoints.end(),
        [&](const Endpoint& item) { return item.id == selected_endpoint_id; });
    if (endpoint != state->endpoints.end()) {
      SendMessageW(
          state->endpoint_combo,
          CB_SETCURSEL,
          static_cast<WPARAM>(
              std::distance(state->endpoints.begin(), endpoint)),
          0);
    }
  }
  update_status(state);
}

void start_update_check(AppState* state, bool silent,
    UpdateResult (*check)() noexcept = check_for_updates) {
  if (state->update_check_in_progress) return;
  if (state->update_thread.joinable()) state->update_thread.join();
  state->pending_update_notice = false;
  state->silent_update_request = silent;
  state->update_check_in_progress = true;
  rebuild_menu(state);
  const HWND window = state->window;
  try {
    state->update_thread = std::thread([state, window, check]() {
      const UpdateResult result = check();
      if (state->shutting_down.load(std::memory_order_acquire)) return;
      { const std::lock_guard<std::mutex> lock(state->update_mutex); state->update_result = result; }
      if (!state->shutting_down.load(std::memory_order_acquire))
        PostMessageW(window, kUpdateResultMessage, 0, 0);
    });
  } catch (const std::system_error&) {
    state->update_check_in_progress = false;
    rebuild_menu(state);
    if (!silent) MessageBoxW(window, ui_strings(state->language).update_failed, L"SoundFork", MB_ICONERROR);
  }
}

void begin_automatic_update_once(AppState* state,
    UpdateResult (*check)() noexcept = check_for_updates) {
  if (!state->automatic_updates_enabled || state->automatic_update_considered) return;
  state->automatic_update_considered = true;
  start_update_check(state, true, check);
}

LRESULT CALLBACK window_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    state = static_cast<AppState*>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(
        window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }
  if (taskbar_created_message != 0 && message == taskbar_created_message) {
    state->tray_icon_added = false;
    (void)add_tray_icon(state);
    return 0;
  }

  switch (message) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      return control_background(wparam, lparam);
    case WM_SHOWWINDOW:
      if (wparam) PostMessageW(window, kAutomaticUpdateMessage, 0, 0);
      break;
    case kAutomaticUpdateMessage:
      if (!IsWindowVisible(window) || IsIconic(window) || !state->automatic_updates_enabled) return 0;
      if (state->pending_update_notice) {
        state->pending_update_notice = false;
        SendMessageW(window, kUpdateResultMessage, 0, 0);
      } else begin_automatic_update_once(state);
      return 0;
    case kShowControlMessage:
      show_control_window(state);
      return 0;
    case kStartSavedRelayMessage:
      start_saved_relay(state);
      update_status(state);
      return 0;
    case WM_CREATE:
      refresh_obs_status(state);
      if (!create_controls(state)) return -1;
      rebuild_menu(state);
      populate_backend_combo(state);
      populate_endpoint_combo(state);
      populate_relay_mode_combo(state);
      (void)add_tray_icon(state);
      update_status(state);
      SetTimer(window, kRefreshTimer, 1000, nullptr);
      return 0;
    case WM_INITMENU:
      if (!state->obs_operation_in_progress) refresh_obs_status(state);
      return 0;
    case WM_GETMINMAXINFO: {
      auto* size = reinterpret_cast<MINMAXINFO*>(lparam);
      size->ptMinTrackSize = {540, 410};
      return 0;
    }
    case WM_SIZE: {
      if (state->status != nullptr && wparam != SIZE_MINIMIZED) layout_controls(state, LOWORD(lparam));
      if (wparam != SIZE_MINIMIZED) PostMessageW(window, kAutomaticUpdateMessage, 0, 0);
      return 0;
    }
    case WM_TIMER:
      if (wparam == kRefreshTimer) {
        if (state->startup_process != nullptr &&
            WaitForSingleObject(state->startup_process, 0) == WAIT_OBJECT_0) {
          DWORD code = 0;
          if (GetExitCodeProcess(state->startup_process, &code) && code != 0 && code != 10) {
            diagnostics::log().write(diagnostics::Level::error, "startup_relay_exit", "exit_code=" + std::to_string(code));
            state->startup_error = std::to_wstring(code);
            notify_startup_error(state);
          }
          CloseHandle(state->startup_process);
          state->startup_process = nullptr;
        }
        if (GetTickCount64() >= state->next_endpoint_probe &&
            SendMessageW(state->endpoint_combo, CB_GETDROPPEDSTATE, 0, 0) == FALSE) {
          state->next_endpoint_probe = GetTickCount64() + 5000;
          populate_endpoint_combo(state);
        }
        update_status(state);
      }
      return 0;
    case kTrayCallbackMessage: {
      const UINT event = LOWORD(lparam);
      if (event == WM_LBUTTONDBLCLK || event == NIN_KEYSELECT) {
        show_control_window(state);
      } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
        show_tray_menu(state);
      }
      return 0;
    }
    case WM_CLOSE:
      if (state->diagnostic_window != nullptr) DestroyWindow(state->diagnostic_window);
      if (!state->force_exit && relay_is_running()) {
        if (state->tray_icon_added || add_tray_icon(state)) {
          ShowWindow(window, SW_HIDE);
          show_tray_notification(state);
        } else {
          ShowWindow(window, SW_MINIMIZE);
        }
        return 0;
      }
      DestroyWindow(window);
      return 0;
    case kObsPluginResultMessage: {
      if (state->obs_thread.joinable()) state->obs_thread.join();
      state->obs_operation_in_progress = false;
      refresh_obs_status(state);
      update_status(state);
      const auto code = state->obs_result.load(std::memory_order_acquire);
      const auto& text = soundfork::control::obs_plugin_strings(state->language == UiLanguage::korean);
      const wchar_t* message_text = text.failed;
      switch (static_cast<obs_plugin::Error>(code)) {
        case obs_plugin::Error::none: message_text = state->obs_operation_install ? text.installed : text.removed; break;
        case obs_plugin::Error::obs_running: message_text = text.close_obs; break;
        case obs_plugin::Error::bundle_missing: message_text = text.missing; break;
        case obs_plugin::Error::unsafe_path: message_text = text.unsafe; break;
        case obs_plugin::Error::rollback_failed: message_text = text.rollback; break;
        case obs_plugin::Error::busy: message_text = text.busy; break;
        default: break;
      }
      if (code == ERROR_CANCELLED) message_text = text.cancelled;
      MessageBoxW(window, message_text, text.menu,
          code == 0 || code == ERROR_CANCELLED ? MB_ICONINFORMATION : MB_ICONWARNING);
      return 0;
    }
    case kUpdateResultMessage: {
      if (state->update_thread.joinable()) {
        state->update_thread.join();
      }
      UpdateResult result;
      {
        const std::lock_guard<std::mutex> lock(state->update_mutex);
        result = state->update_result;
      }
      state->update_check_in_progress = false;
      rebuild_menu(state);
      if (state->silent_update_request) {
        if (!state->automatic_updates_enabled || result.status != UpdateStatus::available) return 0;
        if (!IsWindowVisible(window) || IsIconic(window)) { state->pending_update_notice = true; return 0; }
      }
      const UiStrings& strings = ui_strings(state->language);
      if (result.status == UpdateStatus::available) {
        std::wstring message_text = strings.update_available;
        message_text += result.latest_tag;
        message_text += L"\n";
        message_text += strings.update_current;
        message_text += L"v" SOUNDFORK_VERSION_WIDE;
        const bool korean = state->language == UiLanguage::korean;
        const TASKDIALOG_BUTTON buttons[] = {
            {IDYES, korean ? L"릴리스 페이지 열기" : L"Open release page"},
            {IDNO, korean ? L"나중에" : L"Later"}};
        TASKDIALOGCONFIG dialog{sizeof(dialog)};
        dialog.hwndParent = window;
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        dialog.pszWindowTitle = L"SoundFork";
        dialog.pszMainInstruction = korean ? L"새 버전이 있습니다" : L"A new version is available";
        dialog.pszContent = message_text.c_str();
        dialog.pszVerificationText = korean ? L"앞으로 자동 업데이트 알림 표시 안 함" : L"Don't show automatic update notifications again";
        dialog.cButtons = static_cast<UINT>(std::size(buttons)); dialog.pButtons = buttons;
        dialog.nDefaultButton = IDNO;
        int chosen = IDNO; BOOL disable = FALSE;
        const auto shown = TaskDialogIndirect(&dialog, &chosen, nullptr, &disable);
        if (SUCCEEDED(shown) && disable) {
          const auto saved = soundfork::control::save_automatic_updates(false);
          if (saved == ERROR_SUCCESS) { state->automatic_updates_enabled = false; rebuild_menu(state); }
          else MessageBoxW(window, korean ? L"알림 설정을 저장하지 못했습니다." : L"Could not save notification settings.", L"SoundFork", MB_ICONERROR);
        }
        if (SUCCEEDED(shown) && chosen == IDYES) {
          std::wstring release_url = kReleasesUrl;
          release_url += L"/tag/";
          release_url += result.latest_tag;
          const HINSTANCE opened = ShellExecuteW(
              window,
              L"open",
              release_url.c_str(),
              nullptr,
              nullptr,
              SW_SHOWNORMAL);
          if (reinterpret_cast<INT_PTR>(opened) <= 32) {
            MessageBoxW(
                window,
                strings.open_project_failed,
                L"SoundFork",
                MB_ICONERROR);
          }
        }
      } else {
        MessageBoxW(
            window,
            result.status == UpdateStatus::up_to_date
                ? strings.update_not_available
                : strings.update_failed,
            L"SoundFork",
            result.status == UpdateStatus::up_to_date ? MB_ICONINFORMATION
                                                      : MB_ICONERROR);
      }
      return 0;
    }
    case WM_COMMAND: {
      const int control = LOWORD(wparam);
      if (control >= kNavigate && control < kNavigate + kPageCount) {
        state->panel.page = control - kNavigate;
        SetMenuDefaultItem(GetMenu(window), static_cast<UINT>(state->panel.page), TRUE);
        DrawMenuBar(window);
        if (state->panel.page == 3 && !state->obs_operation_in_progress) refresh_obs_status(state);
        layout_controls(state, 0);
        return 0;
      }
      if (control == kDisplayLanguage && HIWORD(wparam) == CBN_SELCHANGE) {
        apply_language(state, SendMessageW(state->panel.language, CB_GETCURSEL, 0, 0) == 0
            ? UiLanguage::korean : UiLanguage::english, true);
        return 0;
      }
      if (control == kCustomChannels || (control == kChannels && HIWORD(wparam) == CBN_SELCHANGE)) {
        auto pair = state->panel.pair;
        const auto selected = SendMessageW(state->panel.channels, CB_GETCURSEL, 0, 0);
        if (control == kChannels && selected >= 0 && selected < 32) {
          pair = {static_cast<DWORD>(selected) * 2, static_cast<DWORD>(selected) * 2 + 1};
          save_channel_selection(state, pair);
        } else if (control == kCustomChannels || selected == 32) {
          if (choose_channels(state, &pair)) save_channel_selection(state, pair);
          else populate_channels(state);
        }
        return 0;
      }
      if (control == kAutomaticUpdateNotifications) {
        const bool enabled = !state->automatic_updates_enabled;
        if (soundfork::control::save_automatic_updates(enabled) == ERROR_SUCCESS) {
          state->automatic_updates_enabled = enabled;
          if (!enabled) state->pending_update_notice = false;
          rebuild_menu(state);
        } else MessageBoxW(window, state->language == UiLanguage::korean
            ? L"알림 설정을 저장하지 못했습니다." : L"Could not save notification settings.", L"SoundFork", MB_ICONERROR);
        return 0;
      }
      if (control == kShowDiagnostics) { show_diagnostics(state); return 0; }
      if (control == kOpenLogs) {
        const auto directory = diagnostics::log_directory();
        std::error_code error;
        if (!directory.empty()) std::filesystem::create_directories(directory, error);
        if (directory.empty() || error || reinterpret_cast<INT_PTR>(ShellExecuteW(
            window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
          MessageBoxW(window, soundfork::control::diagnostic_strings(state->language == UiLanguage::korean).logs_failed,
              L"SoundFork", MB_ICONWARNING);
        return 0;
      }
      if (control == kEndpointCombo && HIWORD(wparam) == CBN_SELCHANGE) {
        const auto selected = SendMessageW(state->endpoint_combo, CB_GETCURSEL, 0, 0);
        if (selected >= 0 && static_cast<std::size_t>(selected) < state->endpoints.size() &&
            is_broadcast_endpoint(state->endpoints[static_cast<std::size_t>(selected)])) {
          if (!save_endpoint(state, state->endpoints[static_cast<std::size_t>(selected)].id))
            populate_endpoint_combo(state);
        }
        update_status(state);
        return 0;
      }
      if ((control == kRelayEnabled && HIWORD(wparam) == BN_CLICKED) ||
          (control == kRelayBuffer && HIWORD(wparam) == CBN_SELCHANGE)) {
        const bool enabled_change = control == kRelayEnabled;
        const auto selection = SendMessageW(state->relay_buffer, CB_GETCURSEL, 0, 0);
        if (!enabled_change && (selection < 0 ||
            static_cast<std::size_t>(selection) >= std::size(kRelayBuffers))) return 0;
        const DWORD value = enabled_change
            ? (SendMessageW(state->relay_enabled, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1U : 0U)
            : kRelayBuffers[selection];
        std::wstring error;
        if (relay_is_running() && !stop_relay(state->language, &error)) {
          MessageBoxW(window, error.c_str(), L"SoundFork", MB_ICONERROR);
          update_status(state);
          return 0;
        }
        const auto result = soundfork::capture::save_relay_preference(
            enabled_change ? L"RelayEnabled" : L"RelayBufferFrames", value);
        if (enabled_change && result == ERROR_SUCCESS) {
          (void)soundfork::control::RelaySettings{}.apply_startup_preference(executable_path());
        }
        if (result != ERROR_SUCCESS) {
          MessageBoxW(window, state->language == UiLanguage::korean
              ? L"설정을 저장하지 못했습니다." : L"Could not save the setting.",
              L"SoundFork", MB_ICONERROR);
        } else if ((!enabled_change || value != 0) &&
            soundfork::control::RelaySettings{}.mode() == soundfork::control::RelayMode::application) {
          MessageBoxW(window, relay_mode_strings(state->language).restart_game,
              L"SoundFork", MB_ICONINFORMATION);
        }
        update_status(state);
        return 0;
      }
      if (control == kRelayModeCombo && HIWORD(wparam) == CBN_SELCHANGE) {
        const auto selected = SendMessageW(
            state->relay_mode_combo, CB_GETCURSEL, 0, 0);
        const auto mode = selected == 1
            ? soundfork::control::RelayMode::application
            : soundfork::control::RelayMode::entire_screen;
        soundfork::control::RelaySettings settings;
        const auto previous_mode = settings.mode();
        if (mode == soundfork::control::RelayMode::application) {
          const auto endpoint = SendMessageW(
              state->endpoint_combo, CB_GETCURSEL, 0, 0);
          if (endpoint < 0 ||
              static_cast<std::size_t>(endpoint) >= state->endpoints.size() ||
              !is_broadcast_endpoint(
                  state->endpoints[static_cast<std::size_t>(endpoint)])) {
            MessageBoxW(window, ui_strings(state->language).select_endpoint,
                L"SoundFork", MB_ICONWARNING);
            populate_relay_mode_combo(state);
            return 0;
          }
          if (!save_endpoint(
                  state, state->endpoints[static_cast<std::size_t>(endpoint)].id)) {
            populate_relay_mode_combo(state);
            return 0;
          }
        }
        // Changing ownership modes stops whichever relay currently owns the
        // shared singleton. The newly selected application mode starts only
        // on the next ASIO initialization; entire-screen mode remains manual.
        std::wstring stop_error;
        if (relay_is_running() &&
            !stop_relay(state->language, &stop_error)) {
          MessageBoxW(window, stop_error.c_str(), L"SoundFork", MB_ICONERROR);
          populate_relay_mode_combo(state);
          return 0;
        }
        auto result = settings.save_mode(mode);
        if (result == ERROR_SUCCESS) {
          result = settings.apply_startup_preference(executable_path());
        }
        if (result != ERROR_SUCCESS) {
          (void)settings.save_mode(previous_mode);
          (void)settings.apply_startup_preference(executable_path());
          MessageBoxW(window,
              state->language == UiLanguage::korean
                  ? L"공유 방식을 저장하지 못했습니다."
                  : L"Could not save the sharing mode.",
              L"SoundFork", MB_ICONERROR);
          populate_relay_mode_combo(state);
          return 0;
        }
        diagnostics::log().write(diagnostics::Level::info,
            "relay_mode_changed",
            mode == soundfork::control::RelayMode::application
                ? "mode=application" : "mode=entire-screen");
        if (mode == soundfork::control::RelayMode::application) {
          MessageBoxW(window, relay_mode_strings(state->language).restart_game,
              L"SoundFork", MB_ICONINFORMATION);
        }
        update_status(state);
        return 0;
      }
      if (control == kStartupRelay && HIWORD(wparam) == BN_CLICKED) {
        soundfork::control::RelaySettings settings;
        const auto exe = executable_path();
        const bool enable = SendMessageW(state->startup_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (enable) {
          const auto selected = SendMessageW(state->endpoint_combo, CB_GETCURSEL, 0, 0);
          if (selected < 0 || static_cast<std::size_t>(selected) >= state->endpoints.size() ||
              !is_broadcast_endpoint(state->endpoints[static_cast<std::size_t>(selected)])) {
            MessageBoxW(window, ui_strings(state->language).select_endpoint, L"SoundFork", MB_ICONWARNING);
            update_status(state);
            return 0;
          }
          if (!save_endpoint(state, state->endpoints[static_cast<std::size_t>(selected)].id)) {
            update_status(state);
            return 0;
          }
        }
        const auto result = settings.set_startup_preference(exe, enable);
        diagnostics::log().write(result == ERROR_SUCCESS ? diagnostics::Level::info : diagnostics::Level::error,
            "startup_preference", "enabled=" + std::to_string(enable) + " win32=" + std::to_string(result));
        if (result != ERROR_SUCCESS) {
          const auto error = std::wstring(startup_strings(state->language).save_failed) + std::to_wstring(result);
          MessageBoxW(window, error.c_str(), L"SoundFork", MB_ICONERROR);
        }
        rebuild_menu(state);
        update_status(state);
        return 0;
      }
      if (control == kObsInstall || control == kObsRemove) {
        start_obs_operation(state, control == kObsInstall);
        return 0;
      }
      if (control == kObsInformation) {
        const auto& text = soundfork::control::obs_plugin_strings(state->language == UiLanguage::korean);
        const auto description = std::wstring(text.installed_version) + obs_installed_text(state) +
            L"\n\n" + text.usage + obs_plugin::locate(executable_directory()).installed.native();
        MessageBoxW(window, description.c_str(), text.menu, MB_ICONINFORMATION);
        return 0;
      }
      if (control == kLanguageEnglish || control == kLanguageKorean) {
        apply_language(
            state,
            control == kLanguageKorean ? UiLanguage::korean
                                       : UiLanguage::english,
            true);
        return 0;
      }
      const UiStrings& strings = ui_strings(state->language);
      if (control == kTrayOpen) {
        show_control_window(state);
        return 0;
      }
      if (control == kTrayToggleRelay) {
        SendMessageW(
            window, WM_COMMAND, MAKEWPARAM(kToggleRelay, BN_CLICKED), 0);
        return 0;
      }
      if (control == kTrayExitControl) {
        state->force_exit = true;
        DestroyWindow(window);
        return 0;
      }
      if (control == kTrayStopAndExit) {
        std::wstring error;
        if (relay_is_running() && !stop_relay(state->language, &error)) {
          MessageBoxW(window, error.c_str(), L"SoundFork", MB_ICONERROR);
          return 0;
        }
        state->force_exit = true;
        DestroyWindow(window);
        return 0;
      }
      if (control == kCheckForUpdates) {
        start_update_check(state, false);
        return 0;
      }
      if (control == kOpenProject) {
        const HINSTANCE opened = ShellExecuteW(
            window, L"open", kProjectUrl, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(opened) <= 32) {
          MessageBoxW(
              window,
              strings.open_project_failed,
              L"SoundFork",
              MB_ICONERROR);
        }
        return 0;
      }
      if (control == kReportBug) {
        const auto opened = ShellExecuteW(window, L"open",
            L"https://github.com/kjh2623jh/SoundFork/issues/new?template=bug_report.yml",
            nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(opened) <= 32)
          MessageBoxW(window, strings.open_project_failed, L"SoundFork", MB_ICONERROR);
        return 0;
      }
      if (control == kCopyReport) {
        update_status(state);
        const bool copied = copy_diagnostics(state);
        MessageBoxW(window,
            copied ? (state->language == UiLanguage::korean
                ? L"진단 정보를 복사했습니다. 내용을 확인한 뒤 버그 제보 양식에 붙여 넣으세요."
                : L"Diagnostics copied. Review the contents before pasting into the bug report.")
                : (state->language == UiLanguage::korean ? L"복사하지 못했습니다. 다시 시도하세요."
                                                       : L"Copy failed. Please try again."),
            L"SoundFork", MB_OK | (copied ? MB_ICONINFORMATION : MB_ICONERROR));
        return 0;
      }
      if (control == kShowAbout) {
        std::wstring about_text = L"SoundFork v" SOUNDFORK_VERSION_WIDE L"\n\n";
        about_text += strings.about_license;
        MessageBoxW(
            window,
            about_text.c_str(),
            strings.about,
            MB_ICONINFORMATION | MB_OK);
        return 0;
      }
      if (control == kApplyBackend && HIWORD(wparam) == BN_CLICKED) {
        const LRESULT selected =
            SendMessageW(state->backend_combo, CB_GETCURSEL, 0, 0);
        if (selected < 0 ||
            static_cast<std::size_t>(selected) >= state->drivers.size()) {
          MessageBoxW(
              window, strings.select_backend, L"SoundFork", MB_ICONWARNING);
          return 0;
        }
        if (elevate_set_backend(
                window, state->drivers[static_cast<std::size_t>(selected)].clsid)) {
          populate_backend_combo(state);
          update_status(state);
          MessageBoxW(
              window,
              strings.backend_updated,
              L"SoundFork",
              MB_ICONINFORMATION);
        } else {
          MessageBoxW(
              window,
              strings.backend_update_failed,
              L"SoundFork",
              MB_ICONWARNING);
        }
        return 0;
      }
      if (control == kToggleRelay && HIWORD(wparam) == BN_CLICKED) {
        std::wstring error;
        state->startup_error.clear();
        if (state->startup_process != nullptr) {
          CloseHandle(state->startup_process);
          state->startup_process = nullptr;
        }
        if (relay_is_running()) {
          if (!stop_relay(state->language, &error)) {
            MessageBoxW(window, error.c_str(), L"SoundFork", MB_ICONERROR);
          }
        } else if (soundfork::control::RelaySettings{}.mode() ==
                   soundfork::control::RelayMode::application) {
          MessageBoxW(window, relay_mode_strings(state->language).restart_game,
              L"SoundFork", MB_ICONINFORMATION);
        } else {
          if (capture_consumer_is_active()) {
            MessageBoxW(
                window,
                strings.another_consumer,
                L"SoundFork",
                MB_ICONWARNING);
            return 0;
          }
          const LRESULT selected =
              SendMessageW(state->endpoint_combo, CB_GETCURSEL, 0, 0);
          if (selected < 0 ||
              static_cast<std::size_t>(selected) >= state->endpoints.size()) {
            MessageBoxW(
                window,
                strings.select_endpoint,
                L"SoundFork",
                MB_ICONWARNING);
            return 0;
          }
          const Endpoint& endpoint =
              state->endpoints[static_cast<std::size_t>(selected)];
          if (!is_broadcast_endpoint(endpoint)) {
            MessageBoxW(window, strings.select_endpoint, L"SoundFork", MB_ICONWARNING);
            return 0;
          }
          if (!save_endpoint(state, endpoint.id)) return 0;
          if (!start_relay(endpoint, state->language, &error)) {
            MessageBoxW(window, error.c_str(), L"SoundFork", MB_ICONERROR);
          }
        }
        Sleep(100);
        update_status(state);
        return 0;
      }
      if (control == kRefresh && HIWORD(wparam) == BN_CLICKED) {
        refresh_obs_status(state);
        populate_backend_combo(state);
        populate_endpoint_combo(state);
        populate_relay_mode_combo(state);
        update_status(state);
        return 0;
      }
      if (control == kGetVbCable && HIWORD(wparam) == BN_CLICKED) {
        const HINSTANCE result = ShellExecuteW(
            window, L"open", kVbCableUrl, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
          MessageBoxW(
              window,
              strings.open_vb_cable_failed,
              L"SoundFork",
              MB_ICONERROR);
        }
        return 0;
      }
      break;
    }
    case WM_DESTROY:
      if (state->diagnostic_window != nullptr) DestroyWindow(state->diagnostic_window);
      state->shutting_down.store(true, std::memory_order_release);
      KillTimer(window, kRefreshTimer);
      remove_tray_icon(state);
      if (state->panel.heading_font != nullptr) {
        DeleteObject(state->panel.heading_font);
        state->panel.heading_font = nullptr;
      }
      if (state->font != nullptr) {
        DeleteObject(state->font);
        state->font = nullptr;
      }
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

int run_control_app(HINSTANCE instance, int show_command, bool startup_launch) {
  INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES};
  if (!InitCommonControlsEx(&common)) return 2;
  soundfork::control::ControlInstance singleton(L"Local\\SoundFork.Control.v1");
  if (!singleton.valid()) return 2;
  if (!singleton.primary()) {
    if (!startup_launch) {
      // The first process can still be constructing its hidden window.
      for (int retry = 0; retry < 40; ++retry) {
        const auto existing = FindWindowW(kWindowClass, nullptr);
        if (existing != nullptr) {
          DWORD pid = 0;
          GetWindowThreadProcessId(existing, &pid);
          AllowSetForegroundWindow(pid);
          PostMessageW(existing, kShowControlMessage, 0, 0);
          return 0;
        }
        Sleep(25);
      }
    }
    return 0;
  }
  AppState state;
  diagnostics::initialize_log(diagnostics::Component::control, SOUNDFORK_VERSION_STRING);
  diagnostics::log().write(diagnostics::Level::info, "control_start", startup_launch ? "origin=login" : "origin=manual");
  state.language = load_ui_language();
  taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = &window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.hIcon = static_cast<HICON>(LoadImageW(
      instance,
      MAKEINTRESOURCEW(IDI_SOUNDFORK_ICON),
      IMAGE_ICON,
      0,
      0,
      LR_DEFAULTSIZE));
  window_class.hIconSm = static_cast<HICON>(LoadImageW(
      instance,
      MAKEINTRESOURCEW(IDI_SOUNDFORK_ICON),
      IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON),
      GetSystemMetrics(SM_CYSMICON),
      0));
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClass;
  state.tray_icon = window_class.hIconSm;
  if (RegisterClassExW(&window_class) == 0) {
    return 2;
  }
  auto diagnostic_class = window_class;
  diagnostic_class.lpfnWndProc = &diagnostic_proc;
  diagnostic_class.lpszClassName = kDiagnosticClass;
  if (RegisterClassExW(&diagnostic_class) == 0) return 2;
  auto channel_class = window_class;
  channel_class.lpfnWndProc = &channel_dialog;
  channel_class.lpszClassName = kChannelDialogClass;
  if (RegisterClassExW(&channel_class) == 0) return 2;
  HWND window = CreateWindowExW(
      WS_EX_CONTROLPARENT,
      kWindowClass,
      ui_strings(state.language).window_title,
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      540,
      420,
      nullptr,
      nullptr,
      instance,
      &state);
  if (window == nullptr) {
    return 2;
  }
  state.automatic_updates_enabled = soundfork::control::automatic_updates_enabled();
  rebuild_menu(&state);
  ShowWindow(window, startup_launch && state.tray_icon_added ? SW_HIDE : show_command);
  UpdateWindow(window);
  if (startup_launch) PostMessageW(window, kStartSavedRelayMessage, 0, 0);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (state.diagnostic_window != nullptr && IsDialogMessageW(state.diagnostic_window, &message)) continue;
    if (IsDialogMessageW(window, &message)) continue;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  state.shutting_down.store(true, std::memory_order_release);
  if (state.update_thread.joinable()) {
    state.update_thread.join();
  }
  if (state.obs_thread.joinable()) state.obs_thread.join();
  diagnostics::log().write(diagnostics::Level::info, "control_exit", "exit_code=" + std::to_string(message.wParam));
  return static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int show_command) {
  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  const bool self_test = arguments != nullptr && argument_count == 2 &&
                         std::wstring_view(arguments[1]) == L"--self-test";
  const bool startup_launch = arguments != nullptr && argument_count == 2 &&
                              std::wstring_view(arguments[1]) == L"--startup";
  if (arguments != nullptr && argument_count == 2 &&
      std::wstring_view(arguments[1]) == L"--remove-user-startup") {
    LocalFree(arguments);
    return static_cast<int>(soundfork::control::RelaySettings{}.remove_startup(executable_path()));
  }
  const bool endpoint_probe = arguments != nullptr && argument_count == 2 &&
                               std::wstring_view(arguments[1]) ==
                                   L"--has-broadcast-endpoint";
  const bool update_probe = arguments != nullptr && argument_count == 2 &&
                            std::wstring_view(arguments[1]) ==
                                L"--check-update";
  const bool unregister_driver =
      arguments != nullptr && argument_count == 2 &&
      std::wstring_view(arguments[1]) == L"--unregister-driver";
  const bool register_driver =
      arguments != nullptr && argument_count == 4 &&
      std::wstring_view(arguments[1]) == L"--register-driver" &&
      std::wstring_view(arguments[2]) == L"--backend-clsid";
  const std::wstring requested_backend =
      register_driver ? std::wstring(arguments[3]) : std::wstring();
  const bool initialize_language =
      arguments != nullptr && argument_count == 3 &&
      std::wstring_view(arguments[1]) == L"--initialize-language";

  if (arguments != nullptr && argument_count == 2 &&
      (std::wstring_view(arguments[1]) == L"--install-obs-plugin" ||
       std::wstring_view(arguments[1]) == L"--remove-obs-plugin" ||
       std::wstring_view(arguments[1]) == L"--can-manage-obs-plugin")) {
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { LocalFree(arguments); return static_cast<int>(obs_plugin::Error::file_operation); }
    const bool probe = std::wstring_view(arguments[1]) == L"--can-manage-obs-plugin";
    const bool install = std::wstring_view(arguments[1]) == L"--install-obs-plugin";
    LocalFree(arguments);
    int code = static_cast<int>(obs_plugin::Error::file_operation);
    try {
      const auto paths = obs_plugin::locate(executable_directory());
      code = probe ? ((!obs_plugin::inspect(paths).present || !obs_plugin::obs_is_running()) ? 0 : 1)
                   : static_cast<int>(obs_plugin::manage(install, paths).error);
    } catch (...) {}
    CoUninitialize();
    return code;
  }

  if (initialize_language) {
    UiLanguage language{};
    const bool valid = parse_ui_language(arguments[2], &language);
    const bool result = valid && save_language(language, true);
    LocalFree(arguments);
    return result ? 0 : 1;
  }

  if (update_probe) {
    const UpdateResult result = check_for_updates();
    LocalFree(arguments);
    return result.status == UpdateStatus::failed ? 1 : 0;
  }

  if (register_driver || unregister_driver) {
    std::wstring error;
    const bool result = invoke_adjacent_driver_registration(
        register_driver, requested_backend, &error);
    if (arguments != nullptr) {
      LocalFree(arguments);
    }
    return result ? 0 : 1;
  }
  if (arguments != nullptr && argument_count == 3 &&
      std::wstring_view(arguments[1]) == L"--set-backend") {
    std::wstring error;
    const bool result = set_backend(arguments[2], &error, load_ui_language());
    if (!result) {
      MessageBoxW(nullptr, error.c_str(), L"SoundFork", MB_ICONERROR);
    }
    LocalFree(arguments);
    return result ? 0 : 1;
  }
  if (arguments != nullptr) {
    LocalFree(arguments);
  }

  // Run registration is the preference: no second bool can become inconsistent.
  if (startup_launch && !soundfork::control::RelaySettings{}.startup_enabled(executable_path())) return 0;

  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) {
    if (startup_launch) return 2;
    const UiStrings& strings = ui_strings(load_ui_language());
    MessageBoxW(
        nullptr,
        strings.com_initialization_failed,
        L"SoundFork",
        MB_ICONERROR);
    return 2;
  }
  if (endpoint_probe) {
    const bool found = has_broadcast_endpoint();
    CoUninitialize();
    return found ? 0 : 1;
  }
  if (self_test) {
    if (classify_endpoint(L"CABLE Input (VB-Audio Virtual Cable)") !=
            EndpointKind::vb_cable ||
        classify_endpoint(L"VoiceMeeterDesktop(VB-Audio Voicemeeter VAIO)") !=
            EndpointKind::voicemeeter ||
        classify_endpoint(L"Speakers (Physical DAC)") != EndpointKind::other ||
        language_from_windows_ui(MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT)) !=
            UiLanguage::korean ||
        language_from_windows_ui(MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT)) !=
            UiLanguage::english ||
        ui_strings(UiLanguage::korean).apply[0] == L'\0' ||
        ui_strings(UiLanguage::korean).tray_open[0] == L'\0' ||
        soundfork::control::obs_plugin_strings(true).menu[0] == L'\0' ||
        !soundfork::control::update_parser_self_test()) {
      CoUninitialize();
      return 3;
    }
#ifdef SOUNDFORK_EXPECT_OBS_PAYLOAD
    if (!obs_plugin::inspect(obs_plugin::locate(executable_directory())).bundled) {
      CoUninitialize();
      return 4;
    }
#endif
    (void)query_registration();
    (void)enumerate_asio_drivers();
    (void)enumerate_endpoints();
    soundfork::capture::SharedRingObserver observer;
    (void)observer.open();
    CoUninitialize();
    return 0;
  }
  const int result = run_control_app(instance, show_command, startup_launch);
  CoUninitialize();
  return result;
}
