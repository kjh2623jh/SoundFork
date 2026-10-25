#include <iostream>
#include <stdexcept>
#include <string>

#include "control-app/control_status.hpp"

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void check(bool korean) {
  using soundfork::control::ControlStatus;
  using soundfork::control::control_status_text;
  ControlStatus status;
  auto text = control_status_text(status, korean);
  require(text.find(korean ? L"확인 필요" : L"Needs attention") != text.npos, "missing configuration hidden");
  status.asio_configured = true;
  text = control_status_text(status, korean);
  require(text.find(korean ? L"가상 장치를 선택" : L"Select a virtual") != text.npos, "device hint missing");
  status.forwarding_running = true;
  text = control_status_text(status, korean);
  require(text.find(korean ? L"저장한 장치 대기" : L"Waiting for the saved device") != text.npos, "late device not distinguished");
  status.device_available = true;
  text = control_status_text(status, korean);
  require(text.find(korean ? L"오디오 연결 대기" : L"Waiting for an audio connection") != text.npos, "idle source not distinguished");
  status.capture_connected = true;
  text = control_status_text(status, korean);
  require(text.find(korean ? L"프로그램 실행 중" : L"Process running") != text.npos, "running process missing");
  require(text.find(korean ? L"정상 송출" : L"Streaming successfully") == text.npos, "unverified reception claimed");
  status.startup_error = L"test error 5";
  text = control_status_text(status, korean);
  require(text.find(korean ? L"자동 전달 시작 실패" : L"Automatic forwarding failed") != text.npos, "startup error hidden");
  require(text.find(korean ? L"진단 정보" : L"Diagnostics") != text.npos, "error guidance missing");
  status.forwarding_running = false;
  require(control_status_text(status, korean).find(korean ? L"중지됨" : L"Stopped") != text.npos, "stop status missing");
  const auto& strings = soundfork::control::diagnostic_strings(korean);
  require(strings.title[0] && strings.copy[0] && strings.close[0] && strings.copy_failed[0] && strings.open_failed[0],
          "diagnostic translations incomplete");
}
}  // namespace

int main() {
  try { check(false); check(true); std::cout << "Control status and diagnostics translations passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
