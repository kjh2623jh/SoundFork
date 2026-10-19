# SoundFork

**한국어** | [English](README.en.md)

> 이 프로젝트는 AI의 도움을 받아 개발되었습니다. 아키텍처 결정, 코드 검토와
> 실제 하드웨어 검증은 프로젝트 관리자가 수행했습니다.

SoundFork는 Windows x64용 저지연 ASIO 브리지입니다. 게임이나 DAW의 오디오를 기존
하드웨어 ASIO 드라이버와 DAC로 재생하면서, 같은 소리를 가상 오디오 장치로 복제해
Discord와 OBS에서 캡처할 수 있게 합니다.

## 다운로드

[**Windows x64 설치 파일 다운로드**](../../releases/download/v0.1.0-alpha.3/SoundFork-Setup-v0.1.0-alpha.3.exe) ·
[ZIP 다운로드](../../releases/download/v0.1.0-alpha.3/SoundFork-v0.1.0-alpha.3-windows-x64.zip) ·
[모든 릴리스 보기](../../releases)

현재 버전은 미서명 `v0.1.0-alpha.3`입니다. Windows SmartScreen 경고가 표시될 수 있습니다.
설치할 때는 GitHub가 자동 생성한 “Source code”가 아니라 `SoundFork-Setup` 파일을
사용합니다.

## 필요 조건

- Windows 10/11 x64
- DAC 또는 오디오 인터페이스의 64비트 ASIO 드라이버
- Discord·OBS 캡처에 사용할 가상 오디오 재생 엔드포인트

가상 오디오 장치는 [VB-CABLE](https://vb-audio.com/Cable/)을 권장합니다. 이미 설치된
VoiceMeeter의 가상 재생 엔드포인트도 사용할 수 있습니다. SoundFork는 이러한 외부
장치를 포함하거나 제거하지 않으며 Windows 기본 오디오 장치를 변경하지 않습니다.

## 설치 및 재생

1. `SoundFork-Setup-v0.1.0-alpha.3.exe`를 실행합니다.
2. SoundFork가 사용할 하드웨어 ASIO 드라이버를 선택합니다.
3. 게임이나 DAW의 ASIO 드라이버로 **SoundFork**를 선택합니다.
4. 방송이나 녹화가 필요하면 **SoundFork Control**에서 가상 재생 엔드포인트를
   선택하고 **릴레이 시작**을 누릅니다.

제어판을 닫아도 ASIO 재생과 이미 실행 중인 릴레이는 계속 동작합니다. 표시 언어는
상단 **언어/Language** 메뉴에서 변경할 수 있습니다.

## Discord 전체 화면 공유

1. **SoundFork Control**의 방송 출력 엔드포인트로 `CABLE Input`을 선택합니다.
2. 게임이나 DAW를 실행하고 **릴레이 시작**을 누릅니다.
3. Discord에서 애플리케이션 창 대신 **전체 모니터 화면**을 공유합니다.
4. Discord 마이크는 평소 사용하는 실제 마이크를 그대로 선택합니다.

다른 가상 오디오 장치를 사용한다면 그 장치의 가상 재생 엔드포인트를 선택합니다.
Discord 애플리케이션 창 공유는 별도 릴레이의 오디오를 포함하지 않아 지원하지 않습니다.

## OBS 캡처

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
