#pragma once

#include <string>

namespace soundfork::control {

// UI observations, not proof of DAC playback, nonzero PCM or remote reception.
struct ControlStatus {
  bool asio_configured = false;
  bool forwarding_running = false;
  bool device_available = false;
  bool capture_connected = false;
  std::wstring startup_error;
};

inline std::wstring control_status_text(const ControlStatus& status, bool korean) {
  std::wstring text = korean ? L"ASIO 설정: " : L"ASIO configuration: ";
  text += status.asio_configured ? (korean ? L"준비됨" : L"Ready")
                                 : (korean ? L"확인 필요" : L"Needs attention");
  text += korean ? L"\r\n가상 장치로 오디오 전달: " : L"\r\nForwarding to virtual device: ";
  if (!status.forwarding_running) text += korean ? L"중지됨" : L"Stopped";
  else if (!status.device_available) text += korean ? L"저장한 장치 대기 중" : L"Waiting for the saved device";
  else if (!status.capture_connected) text += korean ? L"오디오 연결 대기 중" : L"Waiting for an audio connection";
  else text += korean ? L"프로그램 실행 중" : L"Process running";

  if (!status.startup_error.empty()) {
    text += korean ? L"\r\n자동 전달 시작 실패. 도움말 → 진단 정보를 확인하세요."
                   : L"\r\nAutomatic forwarding failed. Open Help > Diagnostics for details.";
  } else if (!status.asio_configured) {
    text += korean ? L"\r\nASIO 드라이버 설정을 확인하세요." : L"\r\nCheck the ASIO driver configuration.";
  } else if (!status.device_available && !status.forwarding_running) {
    text += korean ? L"\r\n전달하려면 방송용 가상 장치를 선택하세요." : L"\r\nSelect a virtual broadcast device to start forwarding.";
  }
  return text;
}

struct DiagnosticStrings {
  const wchar_t* title;
  const wchar_t* copy;
  const wchar_t* close;
  const wchar_t* copy_failed;
  const wchar_t* open_failed;
  const wchar_t* logs;
  const wchar_t* logs_failed;
};

inline const DiagnosticStrings& diagnostic_strings(bool korean) {
  static constexpr DiagnosticStrings ko{L"진단 정보", L"복사", L"닫기",
      L"진단 정보를 클립보드에 복사하지 못했습니다.", L"진단 창을 열 수 없습니다.",
      L"로그 폴더 열기", L"로그 폴더를 열 수 없습니다."};
  static constexpr DiagnosticStrings en{L"Diagnostics", L"Copy", L"Close",
      L"Could not copy diagnostics to the clipboard.", L"Could not open the diagnostics window.",
      L"Open log folder", L"Could not open the log folder."};
  return korean ? ko : en;
}

}  // namespace soundfork::control
