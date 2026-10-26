# SoundFork

**한국어** | [English](README.en.md)

[![CodeQL](https://github.com/kjh2623jh/SoundFork/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/kjh2623jh/SoundFork/actions/workflows/codeql.yml)
[![Release checks · ClamAV](https://github.com/kjh2623jh/SoundFork/actions/workflows/release.yml/badge.svg)](https://github.com/kjh2623jh/SoundFork/actions/workflows/release.yml)

> [!note]
> 이 프로젝트는 AI의 도움을 받아 개발되었습니다.

SoundFork는 게임이나 DAW의 ASIO 오디오를 기존 DAC로 저지연 재생하면서,
같은 소리를 **Discord 화면 공유**와 **OBS 방송·녹화**에도 전달하는 Windows 프로그램입니다.

## 다운로드

[**Windows x64 설치 파일 다운로드**](../../releases/download/v1.0.1/SoundFork-Setup-v1.0.1.exe) ·
[모든 릴리스 보기](../../releases)

현재 버전은 `v1.0.1`입니다. 실행 파일은 코드 서명되지 않았으므로 Windows SmartScreen 경고가 표시될 수 있습니다.

## 필요 조건

- Windows 10/11 x64
- ASIO를 지원하는 64비트 게임 또는 DAW
- 호환되는 64비트 ASIO 드라이버 사전 설치 — DAC·오디오 인터페이스 제조사 드라이버를 권장합니다.

현재 방송용 오디오 복제는 **48 kHz·스테레오·32비트 정수 ASIO 출력**을 지원합니다.
실사용 확인 환경은 DJMAX RESPECT V + FiiO K11 R2R입니다.

## 설치 및 재생

1. 설치 파일을 실행하고 SoundFork가 **사용할 ASIO 드라이버**를 선택합니다.
2. 게임이나 DAW의 ASIO 드라이버를 **SoundFork**로 설정하고 샘플레이트를 **48 kHz**로 맞춥니다.

제어판은 시작 메뉴의 **SoundFork Control**에서 엽니다. 게임 오디오 재생만 사용할 때는
제어판이나 오디오 전달 프로그램을 실행해 둘 필요가 없습니다.

## Discord 전체 화면 공유

가상 오디오 장치를 별도로 설치해야 하며, [VB-CABLE](https://vb-audio.com/Cable/)을
권장합니다. VB-CABLE 기준 설정은 다음과 같습니다.

1. 제어판의 **방송용 오디오 장치**에서 `CABLE Input`을 선택합니다.
2. **전달 시작**을 누르고 게임이나 DAW에서 소리를 재생합니다.
3. Discord에서 **전체 모니터 화면**을 공유합니다. **애플리케이션 창 공유는 지원하지 않습니다.**

다른 가상 오디오 장치를 사용한다면 `CABLE Input` 대신 해당 장치의 가상 재생 장치를 선택합니다.

오디오 전달 중에 제어판을 닫으면 알림 영역(시스템 트레이)에 남아 전달을 유지합니다.
전달을 끝내려면 트레이 메뉴의 **전달 중지 후 종료**를 선택합니다.

## OBS 방송·녹화

### 전용 플러그인 사용

OBS Studio **32.2.2 이상 x64**가 필요합니다.

1. OBS를 종료하고 SoundFork 설치 시 **OBS 오디오 소스 플러그인 설치**를 선택합니다.
   이미 SoundFork를 설치했다면 제어판의 **설정 → OBS 플러그인 → 설치**를 사용합니다.
2. OBS를 실행하고 소스 목록에서 **SoundFork 오디오**를 추가합니다.
3. 다른 장면에서는 새 소스를 만들지 말고 기존 SoundFork 소스를 추가합니다.

OBS-ASIO의 **ASIO Input Capture**가 아닌 **SoundFork 오디오**를 선택합니다.

전용 플러그인을 사용한다면 **가상 오디오 장치와 오디오 전달 기능은 필요하지 않습니다.**

### 가상 오디오 장치 사용

1. 제어판에서 `CABLE Input`을 선택하고 **전달 시작**을 누릅니다.
2. OBS에 **오디오 출력 캡처** 소스를 추가하고 같은 `CABLE Input`을 선택합니다.
   **설정 → 오디오 → 데스크탑 오디오**에서 선택해도 됩니다.

다른 가상 장치라면 SoundFork와 OBS에서 같은 가상 재생 장치를 선택합니다.

**OBS에서 같은 소리를 전용 플러그인과 가상 장치로 동시에 캡처하면 중복되므로 한 가지 방식만 사용합니다.**

## 업데이트·설정·제거

업데이트하거나 제거하기 전에는 게임·DAW와 OBS를 종료하고 오디오 전달을 중지합니다.

- **업데이트:** 제어판의 **도움말 → 업데이트 확인**에서 새 버전을 확인하고 설치 파일을 실행합니다.
  OBS 플러그인도 사용 중이라면 설치 시 플러그인 항목을 선택해 함께 업데이트합니다.
- **언어 변경:** 제어판의 **설정 → 언어**에서 한국어 또는 영어를 선택합니다.
- **자동 시작:** 장치 선택은 자동으로 저장됩니다. 메인 화면의 **Windows 로그인 시 오디오 전달 시작**을
  체크하면 다음 로그인부터 저장한 가상 장치로 전달을 시작합니다. 신규 설치는 기본 꺼짐이며 업데이트는 기존 선택을 유지합니다.
- **상세 상태 확인:** **도움말 → 진단 정보**에서 연결 정보와 통계를 확인하고 복사합니다.
  문제 발생 시 **도움말 → 로그 폴더 열기**에서 진단 로그를 확인할 수 있습니다.
- **OBS 플러그인 관리:** 제어판의 **설정 → OBS 플러그인**에서 설치·업데이트·제거합니다.
  OBS를 종료한 상태에서 진행하며 관리자 권한 승인이 필요합니다.
- **제거:** Windows **설정 → 앱**에서 SoundFork를 제거하거나 시작 메뉴의 **Uninstall SoundFork**를 실행합니다.
  선택한 ASIO 드라이버와 가상 오디오 장치는 제거되지 않습니다.

## 프로젝트 정보

이 프로젝트는 AI의 도움을 받아 개발되었습니다. GPL-3.0-only로 배포됩니다.

[상세 설명](docs/DETAILS.md) · [개발 안내](docs/DEVELOPMENT.md) · [라이선스](LICENSE) ·
[제3자 구성 요소 고지](THIRD_PARTY_NOTICES.md)
