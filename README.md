# SoundFork

**한국어** | [English](README.en.md)

> 이 프로젝트는 AI의 도움을 받아 개발되었습니다. 아키텍처 결정, 코드 검토와
> 실제 하드웨어 검증은 프로젝트 관리자가 수행했습니다.

SoundFork는 Windows x64용 저지연 ASIO 브리지입니다. 게임이나 DAW의 오디오를 기존
하드웨어 ASIO 드라이버와 DAC로 재생하면서, 같은 소리를 Discord와 OBS에서 캡처할 수
있게 합니다. Discord에는 가상 오디오 장치로 릴레이하며, OBS에는 전용 플러그인으로
직접 전달하거나 가상 오디오 장치를 사용합니다.

## 다운로드

[**Windows x64 설치 파일 다운로드**](../../releases/download/v0.1.0-beta.2/SoundFork-Setup-v0.1.0-beta.2.exe) ·
[ZIP 다운로드](../../releases/download/v0.1.0-beta.2/SoundFork-v0.1.0-beta.2-windows-x64.zip) ·
[모든 릴리스 보기](../../releases)

현재 버전은 미서명 `v0.1.0-beta.2`입니다. Windows SmartScreen 경고가 표시될 수 있습니다.
설치할 때는 GitHub가 자동 생성한 “Source code”가 아니라 `SoundFork-Setup` 파일을
사용합니다.

## 필요 조건

- Windows 10/11 x64
- DAC 또는 오디오 인터페이스의 64비트 ASIO 드라이버
- Discord 화면 공유 또는 OBS의 가상 장치 캡처를 사용할 경우 가상 오디오 재생 엔드포인트
- OBS 전용 플러그인을 사용할 경우 OBS Studio 32.2.2 이상 x64

가상 오디오 장치는 [VB-CABLE](https://vb-audio.com/Cable/)을 권장합니다. 이미 설치된
VoiceMeeter의 가상 재생 엔드포인트도 사용할 수 있습니다. SoundFork는 이러한 외부
장치를 포함하거나 제거하지 않으며 Windows 기본 오디오 장치를 변경하지 않습니다.

## 설치 및 재생

1. `SoundFork-Setup-v0.1.0-beta.2.exe`를 실행합니다.
2. SoundFork가 사용할 하드웨어 ASIO 드라이버를 선택합니다.
3. 게임이나 DAW의 ASIO 드라이버로 **SoundFork**를 선택합니다.
4. Discord 화면 공유나 가상 장치 캡처를 사용하려면 **SoundFork Control**에서 가상
   재생 엔드포인트를 선택하고 **릴레이 시작**을 누릅니다. OBS 전용 플러그인만
   사용한다면 릴레이를 시작하지 않아도 됩니다.

기존 버전에서 업데이트할 때는 게임·DAW와 OBS를 종료하고 릴레이를 중지한 뒤
설치 파일을 실행합니다. OBS 플러그인과 릴레이는 같은 버전의 구성 요소를 사용합니다.

Relay가 중지된 상태에서 제어판을 닫으면 제어판이 종료됩니다. Relay가 실행 중이면
알림 영역으로 숨겨지며, 트레이 메뉴에서 제어판을 다시 열거나 Relay를 중지할 수
있습니다. 표시 언어는 **설정 → 언어**에서 변경할 수 있으며, 새 버전은
**도움말 → 업데이트 확인**에서 확인할 수 있습니다.

## Discord 전체 화면 공유

1. **SoundFork Control**의 방송 출력 엔드포인트로 `CABLE Input`을 선택합니다.
2. 게임이나 DAW를 실행하고 **릴레이 시작**을 누릅니다.
3. Discord에서 애플리케이션 창 대신 **전체 모니터 화면**을 공유합니다.
4. Discord 마이크는 평소 사용하는 실제 마이크를 그대로 선택합니다.

다른 가상 오디오 장치를 사용한다면 그 장치의 가상 재생 엔드포인트를 선택합니다.
Discord 애플리케이션 창 공유는 별도 릴레이의 오디오를 포함하지 않아 지원하지 않습니다.

## OBS 캡처

`v0.1.0-beta.2`에는 선택 설치할 수 있는 **SoundFork 오디오** 플러그인이 포함됩니다.

설치 파일에서 **OBS 오디오 소스 플러그인 설치**를 선택한 뒤 OBS를 다시 실행하고,
소스 목록에 **SoundFork 오디오**를 추가합니다. OBS Studio 32.2.2 이상과 스테레오
48 kHz 캡처를 지원합니다. 이 방식만 사용한다면 가상 오디오 장치와 릴레이 실행은
필요하지 않습니다. Discord와 동시에 사용할 때는 기존처럼 릴레이를 시작합니다.
다른 장면에는 새 소스를 만들지 말고 기존 SoundFork 소스를 추가합니다.

나중에 설치하거나 업데이트·제거하려면 **SoundFork Control → 설정 → OBS 플러그인**을
사용합니다. 설치된 버전과 포함된 버전을 확인할 수 있습니다. 파일 작업 전에는 OBS를
종료해야 하며 관리자 권한 승인이 필요합니다. 볼륨과 음소거는 OBS에서 조절합니다.

아래의 기존 가상 장치 캡처 방식도 계속 사용할 수 있습니다. 플러그인과 같은 게임
소리를 동시에 캡처하면 소리가 중복되므로 한 가지 방식을 선택합니다.

1. **SoundFork Control**에서 `CABLE Input`을 선택하고 릴레이를 시작합니다.
2. OBS 장면에 **오디오 출력 캡처** 소스를 추가하고 `CABLE Input`을 선택합니다.

OBS **설정 → 오디오 → 데스크탑 오디오**에서 `CABLE Input`을 선택해도 됩니다. 다른
가상 장치를 사용한다면 SoundFork에서 선택한 것과 동일한 가상 재생 엔드포인트를
OBS에서도 선택합니다.

VB-CABLE의 `CABLE Output`은 녹음 엔드포인트이므로 OBS의 데스크탑 오디오나 오디오
출력 캡처 목록에 나타나지 않으며 이 구성에는 필요하지 않습니다.

## 제거

Windows **설정 → 앱 → 설치된 앱 → SoundFork → 제거** 또는 시작 메뉴의
**Uninstall SoundFork**를 사용합니다. 하드웨어 ASIO 드라이버와 가상 오디오 장치는
제거되지 않습니다.

## 추가 정보

구조, 빌드, 테스트와 알려진 기술적 제한은 [개발자 문서](docs/DEVELOPMENT.md)를
참고하시기 바랍니다. SoundFork는 GPL-3.0-only로 배포됩니다. [LICENSE](LICENSE)와
[제3자 구성 요소 고지](THIRD_PARTY_NOTICES.md)도 함께 확인하시기 바랍니다.
