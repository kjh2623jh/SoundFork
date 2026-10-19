#include <windows.h>
#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
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
#include "capture-backend/shared_ring.hpp"
#include "control-app/resource.h"
#include "control-app/update_checker.hpp"
#include "soundfork_version.hpp"

namespace {

using Microsoft::WRL::ComPtr;
using soundfork::control::UpdateResult;
using soundfork::control::UpdateStatus;
using soundfork::control::check_for_updates;

constexpr wchar_t kWindowClass[] = L"SoundForkControlWindow";
constexpr wchar_t kSoundForkClsid[] =
    L"{A04A0128-E43C-4D08-B701-87EC1A847A89}";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kUpdateResultMessage = WM_APP + 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 2;
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
constexpr int kCheckForUpdates = 2101;
constexpr int kOpenProject = 2102;
constexpr int kShowAbout = 2103;
constexpr int kTrayOpen = 2201;
constexpr int kTrayToggleRelay = 2202;
constexpr int kTrayExitControl = 2203;
constexpr int kTrayStopAndExit = 2204;
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
      L"Stop relay and exit",
      L"SoundFork is still running",
      L"The broadcast relay is still running in the notification area.",
      L"Hardware ASIO backend",
      L"Apply",
      L"Broadcast render endpoint (choose a virtual endpoint)",
      L"Start relay",
      L"Stop relay",
      L"Refresh devices",
      L"Get VB-CABLE",
      L"VB-CABLE detected",
      L"The control app is optional for ASIO playback. Closing it does not stop an active relay.",
      L"Recommended",
      L"Supported",
      L"Ready",
      L"Not ready",
      L"Not configured",
      L"Not registered",
      L"ASIO driver",
      L"Backend",
      L"Driver DLL",
      L"ASIO stream",
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
      L"Discord relay",
      L"Running",
      L"Stopped",
      L"Discord mode: Entire-screen share only",
      L"Application-window share excludes the separate relay process.",
      L"soundfork_discord_relay.exe was not found next to the control app.",
      L"Could not start the Discord relay (Windows error ",
      L"The relay stop channel was not found.",
      L"Could not signal the relay to stop.",
      L"Select an ASIO backend first.",
      L"Backend updated. Restart the ASIO host if it is already open.",
      L"Backend update was cancelled or failed.",
      L"Another capture consumer is active. Stop soundfork_capture_monitor or the other consumer first.",
      L"Select a virtual render endpoint first.",
      L"Could not open the official VB-CABLE download page.",
      L"COM initialization failed.",
      L"The selected ASIO driver is no longer registered.",
      L"SoundFork is not registered or administrator access was denied.",
      L"Could not update the SoundFork backend."};
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
      L"릴레이 중지 후 종료",
      L"SoundFork가 계속 실행 중입니다",
      L"방송 릴레이가 알림 영역에서 계속 실행 중입니다.",
      L"하드웨어 ASIO 백엔드",
      L"적용",
      L"방송 출력 엔드포인트 (가상 장치 선택)",
      L"릴레이 시작",
      L"릴레이 중지",
      L"장치 새로 고침",
      L"VB-CABLE 받기",
      L"VB-CABLE 감지됨",
      L"제어판을 닫아도 ASIO 재생과 실행 중인 릴레이는 계속 동작합니다.",
      L"권장",
      L"지원됨",
      L"준비됨",
      L"준비되지 않음",
      L"설정되지 않음",
      L"등록되지 않음",
      L"ASIO 드라이버",
      L"백엔드",
      L"드라이버 DLL",
      L"ASIO 스트림",
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
      L"Discord 릴레이",
      L"실행 중",
      L"중지됨",
      L"Discord 모드: 전체 화면 공유 전용",
      L"애플리케이션 창 공유에는 별도 릴레이 프로세스의 오디오가 포함되지 않습니다.",
      L"제어판과 같은 폴더에서 soundfork_discord_relay.exe를 찾을 수 없습니다.",
      L"Discord 릴레이를 시작할 수 없습니다 (Windows 오류 ",
      L"릴레이 종료 채널을 찾을 수 없습니다.",
      L"릴레이에 종료 신호를 보낼 수 없습니다.",
      L"먼저 ASIO 백엔드를 선택하세요.",
      L"백엔드를 변경했습니다. ASIO 호스트가 열려 있다면 다시 시작하세요.",
      L"백엔드 변경이 취소됐거나 실패했습니다.",
      L"다른 캡처 소비자가 실행 중입니다. soundfork_capture_monitor 또는 다른 소비자를 먼저 종료하세요.",
      L"먼저 가상 출력 엔드포인트를 선택하세요.",
      L"VB-CABLE 공식 다운로드 페이지를 열 수 없습니다.",
      L"COM 초기화에 실패했습니다.",
      L"선택한 ASIO 드라이버가 더 이상 등록되어 있지 않습니다.",
      L"SoundFork가 등록되어 있지 않거나 관리자 권한이 거부됐습니다.",
      L"SoundFork 백엔드를 변경할 수 없습니다."};
  return language == UiLanguage::korean ? korean : english;
}

struct Endpoint {
  std::wstring name;
  std::wstring id;
  EndpointKind kind = EndpointKind::other;
};

struct RegistrationStatus {
  bool asio_registered = false;
  bool com_registered = false;
  bool dll_exists = false;
  std::wstring backend_clsid;
  std::wstring driver_dll;
};

struct AppState {
  HWND window = nullptr;
  HWND backend_combo = nullptr;
  HWND endpoint_combo = nullptr;
  HWND relay_button = nullptr;
  HWND vb_cable_button = nullptr;
  HWND status = nullptr;
  HWND backend_label = nullptr;
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
  std::mutex update_mutex;
  soundfork::control::UpdateResult update_result;
  std::thread update_thread;
  HICON tray_icon = nullptr;
  bool tray_icon_added = false;
  bool tray_notification_shown = false;
  bool force_exit = false;
};

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
  return observer.open() && observer.snapshot(&status) &&
         status.consumer_active;
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
    const Endpoint& endpoint, UiLanguage language, std::wstring* error) {
  const UiStrings& strings = ui_strings(language);
  const std::wstring relay_path =
      executable_directory() + L"\\soundfork_discord_relay.exe";
  if (GetFileAttributesW(relay_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    *error = strings.relay_missing;
    return false;
  }
  std::wstring command = quote_argument(relay_path) + L" --endpoint " +
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
    *error = std::wstring(strings.relay_start_failed) +
             std::to_wstring(GetLastError()) + L").";
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool stop_relay(UiLanguage language, std::wstring* error) {
  const UiStrings& strings = ui_strings(language);
  HANDLE event = OpenEventW(
      EVENT_MODIFY_STATE, FALSE, soundfork::capture::kRelayStopEventName);
  if (event == nullptr) {
    *error = strings.relay_stop_missing;
    return false;
  }
  const bool result = SetEvent(event) != FALSE;
  CloseHandle(event);
  if (!result) {
    *error = strings.relay_stop_failed;
  }
  return result;
}

bool set_backend(
    std::wstring_view requested_clsid,
    std::wstring* error,
    UiLanguage language = UiLanguage::english) {
  const UiStrings& strings = ui_strings(language);
  const std::vector<AsioDriver> drivers = enumerate_asio_drivers();
  const auto selected = std::find_if(
      drivers.begin(), drivers.end(), [&](const AsioDriver& driver) {
        return equals_ignore_case(driver.clsid, requested_clsid);
      });
  if (selected == drivers.end()) {
    *error = strings.backend_missing;
    return false;
  }
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
    if (!backend_is_registered) {
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

void set_control_font(HWND control, HFONT font) {
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void populate_backend_combo(AppState* state) {
  const RegistrationStatus registration = query_registration();
  state->drivers = enumerate_asio_drivers();
  SendMessageW(state->backend_combo, CB_RESETCONTENT, 0, 0);
  int selected_index = -1;
  for (std::size_t index = 0; index < state->drivers.size(); ++index) {
    SendMessageW(
        state->backend_combo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(state->drivers[index].name.c_str()));
    if (equals_ignore_case(
            state->drivers[index].clsid, registration.backend_clsid)) {
      selected_index = static_cast<int>(index);
    }
  }
  if (selected_index >= 0) {
    SendMessageW(state->backend_combo, CB_SETCURSEL, selected_index, 0);
  }
}

std::wstring read_saved_endpoint() {
  RegistryKey key;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\SoundFork",
          0,
          KEY_READ,
          &key.value) != ERROR_SUCCESS) {
    return {};
  }
  return read_registry_string(key.value, L"RelayEndpointId");
}

void save_endpoint(std::wstring_view endpoint_id) {
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
    return;
  }
  RegSetValueExW(
      key.value,
      L"RelayEndpointId",
      0,
      REG_SZ,
      reinterpret_cast<const BYTE*>(endpoint_id.data()),
      static_cast<DWORD>((endpoint_id.size() + 1) * sizeof(wchar_t)));
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
  if (selected_index < 0) {
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
  if (observer.open() && observer.snapshot(&stream) && stream.producer_active) {
    text << strings.asio_stream << L": " << strings.active << L" (PID "
         << stream.producer_pid << L")\r\n";
    text << strings.format << L": " << stream.format.sample_rate << L" Hz, "
         << stream.format.channel_count << L" " << strings.channels << L", "
         << stream.format.frames_per_block << L" " << strings.frames_per_block
         << L"\r\n";
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
  }

  const bool relay_running = relay_is_running();
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
  text << strings.discord_mode << L"\r\n";
  text << strings.application_share_note;
  SetWindowTextW(state->status, text.str().c_str());
  SetWindowTextW(
      state->relay_button,
      relay_running ? strings.stop_relay : strings.start_relay);
  update_tray_icon(state);
}

void create_controls(AppState* state) {
  const UiStrings& strings = ui_strings(state->language);
  state->font = CreateFontW(
      -16,
      0,
      0,
      0,
      FW_NORMAL,
      FALSE,
      FALSE,
      FALSE,
      DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY,
      DEFAULT_PITCH,
      L"Segoe UI");
  state->backend_label = CreateWindowExW(
      0,
      L"STATIC",
      strings.backend_label,
      WS_CHILD | WS_VISIBLE,
      18,
      14,
      180,
      22,
      state->window,
      nullptr,
      nullptr,
      nullptr);
  state->backend_combo = CreateWindowExW(
      0,
      L"COMBOBOX",
      nullptr,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
      18,
      37,
      470,
      220,
      state->window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBackendCombo)),
      nullptr,
      nullptr);
  state->apply_button = CreateWindowExW(
      0,
      L"BUTTON",
      strings.apply,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      500,
      36,
      100,
      28,
      state->window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyBackend)),
      nullptr,
      nullptr);
  state->endpoint_label = CreateWindowExW(
      0,
      L"STATIC",
      strings.endpoint_label,
      WS_CHILD | WS_VISIBLE,
      18,
      75,
      440,
      22,
      state->window,
      nullptr,
      nullptr,
      nullptr);
  state->endpoint_combo = CreateWindowExW(
      0,
      L"COMBOBOX",
      nullptr,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
      18,
      98,
      470,
      240,
      state->window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEndpointCombo)),
      nullptr,
      nullptr);
  state->relay_button = CreateWindowExW(
      0,
      L"BUTTON",
      strings.start_relay,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      500,
      97,
      100,
      28,
      state->window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToggleRelay)),
      nullptr,
      nullptr);
  state->refresh_button = CreateWindowExW(
      0,
      L"BUTTON",
      strings.refresh_devices,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      18,
      139,
      130,
      28,
      state->window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefresh)),
      nullptr,
      nullptr);
  state->vb_cable_button = CreateWindowExW(
      0,
      L"BUTTON",
      strings.get_vb_cable,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      160,
      139,
      145,
      28,
      state->window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGetVbCable)),
      nullptr,
      nullptr);
  state->note = CreateWindowExW(
      0,
      L"STATIC",
      strings.note,
      WS_CHILD | WS_VISIBLE,
      317,
      144,
      353,
      42,
      state->window,
      nullptr,
      nullptr,
      nullptr);
  state->status = CreateWindowExW(
      WS_EX_CLIENTEDGE,
      L"EDIT",
      nullptr,
      WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_READONLY |
          ES_AUTOVSCROLL | WS_VSCROLL,
      18,
      189,
      652,
      261,
      state->window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatus)),
      nullptr,
      nullptr);
  for (HWND control : {state->backend_label,
                       state->backend_combo,
                       state->apply_button,
                       state->endpoint_label,
                       state->endpoint_combo,
                       state->relay_button,
                       state->refresh_button,
                       state->vb_cable_button,
                       state->note,
                       state->status}) {
    set_control_font(control, state->font);
  }
}

void rebuild_menu(AppState* state) {
  HMENU previous = GetMenu(state->window);
  HMENU menu = CreateMenu();
  HMENU settings = CreatePopupMenu();
  HMENU language = CreatePopupMenu();
  HMENU help = CreatePopupMenu();
  if (menu == nullptr || settings == nullptr || language == nullptr ||
      help == nullptr) {
    if (menu != nullptr) {
      DestroyMenu(menu);
    }
    if (settings != nullptr) {
      DestroyMenu(settings);
    }
    if (language != nullptr) {
      DestroyMenu(language);
    }
    if (help != nullptr) {
      DestroyMenu(help);
    }
    return;
  }
  AppendMenuW(language, MF_STRING, kLanguageEnglish, L"English");
  AppendMenuW(language, MF_STRING, kLanguageKorean, L"한국어");
  CheckMenuRadioItem(
      language,
      kLanguageEnglish,
      kLanguageKorean,
      state->language == UiLanguage::korean ? kLanguageKorean
                                            : kLanguageEnglish,
      MF_BYCOMMAND);
  AppendMenuW(
      settings,
      MF_POPUP,
      reinterpret_cast<UINT_PTR>(language),
      ui_strings(state->language).language_menu);
  AppendMenuW(
      menu,
      MF_POPUP,
      reinterpret_cast<UINT_PTR>(settings),
      ui_strings(state->language).settings_menu);
  AppendMenuW(
      help,
      MF_STRING | (state->update_check_in_progress ? MF_GRAYED : 0),
      kCheckForUpdates,
      ui_strings(state->language).check_for_updates);
  AppendMenuW(help, MF_STRING, kOpenProject, ui_strings(state->language).open_project);
  AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(help, MF_STRING, kShowAbout, ui_strings(state->language).about);
  AppendMenuW(
      menu,
      MF_POPUP,
      reinterpret_cast<UINT_PTR>(help),
      ui_strings(state->language).help_menu);
  SetMenu(state->window, menu);
  DrawMenuBar(state->window);
  if (previous != nullptr) {
    DestroyMenu(previous);
  }
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
  SetWindowTextW(state->backend_label, strings.backend_label);
  SetWindowTextW(state->apply_button, strings.apply);
  SetWindowTextW(state->endpoint_label, strings.endpoint_label);
  SetWindowTextW(state->refresh_button, strings.refresh_devices);
  SetWindowTextW(state->note, strings.note);
  rebuild_menu(state);
  populate_endpoint_combo(state);
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
    case WM_CREATE:
      create_controls(state);
      rebuild_menu(state);
      populate_backend_combo(state);
      populate_endpoint_combo(state);
      (void)add_tray_icon(state);
      update_status(state);
      SetTimer(window, kRefreshTimer, 1000, nullptr);
      return 0;
    case WM_SIZE: {
      const int width = LOWORD(lparam);
      const int height = HIWORD(lparam);
      if (state->status != nullptr) {
        MoveWindow(
            state->status,
            18,
            189,
            std::max(200, width - 36),
            std::max(120, height - 207),
            TRUE);
      }
      return 0;
    }
    case WM_TIMER:
      if (wparam == kRefreshTimer) {
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
      const UiStrings& strings = ui_strings(state->language);
      if (result.status == UpdateStatus::available) {
        std::wstring message_text = strings.update_available;
        message_text += result.latest_tag;
        message_text += L"\n";
        message_text += strings.update_current;
        message_text += L"v" SOUNDFORK_VERSION_WIDE;
        message_text += strings.update_open_question;
        if (MessageBoxW(
                window,
                message_text.c_str(),
                L"SoundFork",
                MB_ICONINFORMATION | MB_YESNO) == IDYES) {
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
        if (state->update_check_in_progress) {
          return 0;
        }
        if (state->update_thread.joinable()) {
          state->update_thread.join();
        }
        state->update_check_in_progress = true;
        rebuild_menu(state);
        try {
          state->update_thread = std::thread([state, window]() {
            const UpdateResult result = check_for_updates();
            if (state->shutting_down.load(std::memory_order_acquire)) {
              return;
            }
            {
              const std::lock_guard<std::mutex> lock(state->update_mutex);
              state->update_result = result;
            }
            if (!state->shutting_down.load(std::memory_order_acquire)) {
              PostMessageW(window, kUpdateResultMessage, 0, 0);
            }
          });
        } catch (const std::system_error&) {
          state->update_check_in_progress = false;
          rebuild_menu(state);
          MessageBoxW(
              window, strings.update_failed, L"SoundFork", MB_ICONERROR);
        }
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
        if (relay_is_running()) {
          if (!stop_relay(state->language, &error)) {
            MessageBoxW(window, error.c_str(), L"SoundFork", MB_ICONERROR);
          }
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
          save_endpoint(endpoint.id);
          if (!start_relay(endpoint, state->language, &error)) {
            MessageBoxW(window, error.c_str(), L"SoundFork", MB_ICONERROR);
          }
        }
        Sleep(100);
        update_status(state);
        return 0;
      }
      if (control == kRefresh && HIWORD(wparam) == BN_CLICKED) {
        populate_backend_combo(state);
        populate_endpoint_combo(state);
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
      state->shutting_down.store(true, std::memory_order_release);
      KillTimer(window, kRefreshTimer);
      remove_tray_icon(state);
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

int run_control_app(HINSTANCE instance, int show_command) {
  AppState state;
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
  HWND window = CreateWindowExW(
      0,
      kWindowClass,
      ui_strings(state.language).window_title,
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      710,
      510,
      nullptr,
      nullptr,
      instance,
      &state);
  if (window == nullptr) {
    return 2;
  }
  ShowWindow(window, show_command);
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  state.shutting_down.store(true, std::memory_order_release);
  if (state.update_thread.joinable()) {
    state.update_thread.join();
  }
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

  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) {
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
        !soundfork::control::update_parser_self_test()) {
      CoUninitialize();
      return 3;
    }
    (void)query_registration();
    (void)enumerate_asio_drivers();
    (void)enumerate_endpoints();
    soundfork::capture::SharedRingObserver observer;
    (void)observer.open();
    CoUninitialize();
    return 0;
  }
  const int result = run_control_app(instance, show_command);
  CoUninitialize();
  return result;
}
