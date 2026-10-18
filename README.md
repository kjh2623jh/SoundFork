# SoundFork

**한국어** | [English](README.en.md)

> 이 프로젝트는 AI의 도움을 받아 개발되었습니다. 아키텍처 결정, 코드 검토,
> 실제 하드웨어 및 Discord 검증은 프로젝트 관리자가 수행했습니다.

Windows x64용 저지연 ASIO 프록시다. 게임 소리는 실제 하드웨어 ASIO 드라이버와
DAC로 직접 전달하면서, 같은 오디오를 Discord 전체 화면 공유용으로 복제한다.

## 다운로드

[**최신 릴리스 보기**](../../releases) ·
[**Windows x64 설치 파일 바로 다운로드**](../../releases/download/v0.1.0-alpha.1/SoundFork-Setup-v0.1.0-alpha.1.exe) ·
[ZIP 다운로드](../../releases/download/v0.1.0-alpha.1/SoundFork-v0.1.0-alpha.1-windows-x64.zip)

현재 버전은 `v0.1.0-alpha.1` 미서명 Alpha다. Windows SmartScreen 경고가 표시될
수 있으며, Release에 함께 제공되는 `SHA256SUMS.txt`로 파일을 확인할 수 있다.
GitHub의 자동 생성 “Source code” ZIP은 실행 가능한 Windows 배포판이 아니다.

## 주요 기능

- 하드웨어 ASIO 드라이버 버퍼를 직접 사용해 재생 경로의 추가 버퍼를 방지
- 방송 소비자가 느리거나 종료돼도 DAC 재생을 멈추지 않는 비차단 오디오 복제
- FiiO를 포함한 등록된 64비트 ASIO 드라이버 선택
- VB-CABLE 우선, VoiceMeeter 대안 자동 감지
- 백엔드 상태, 스트림 정보와 드롭 수를 보여주는 SoundFork Control
- Windows UI 언어를 따르는 영어·한국어 제어판과 저장되는 언어 선택
- 설치 프로그램과 시작 메뉴 제거 지원

SoundFork는 DJMAX 전용 프로그램이 아니다. 표준 64비트 ASIO를 지원하는 다른 게임과
DAW에서도 사용할 수 있다. 현재 실제 검증 환경은 DJMAX RESPECT V, FiiO K11 R2R,
48 kHz / 64 samples다.

## 필요 조건

- Windows 10/11 x64
- DAC 또는 오디오 인터페이스의 64비트 ASIO 드라이버
- Discord 방송에는 [VB-CABLE](https://vb-audio.com/Cable/) 권장

SoundFork는 FiiO, VB-CABLE 또는 VoiceMeeter를 포함하거나 제거하지 않으며 Windows
기본 재생·녹음 장치도 변경하지 않는다.

## 설치

1. Release에서 `SoundFork-Setup-vX.Y.Z.exe`를 내려받아 실행한다.
2. SoundFork가 감쌀 하드웨어 ASIO 드라이버를 선택한다. `FiiO ASIO Driver`가 있으면
   기본 선택된다.
3. 설치가 끝나면 **SoundFork Control**을 연다.
4. 게임이나 DAW의 ASIO 드라이버로 **SoundFork**를 선택한다.

컨트롤 앱은 재생에 필수 요소가 아니다. 창을 닫아도 게임의 DAC 출력과 이미 실행
중인 릴레이는 계속 동작한다. 상단 **Language/언어** 메뉴에서 표시 언어를 즉시
바꿀 수 있으며 다음 실행에도 유지된다.

## Discord 전체 화면 공유

```text
게임 -> SoundFork -> FiiO ASIO -> DAC
                  +-> 릴레이 -> VB-CABLE -> Discord 전체 화면 공유
```

1. VB-CABLE을 설치한 뒤 공급사 안내에 따라 재부팅한다.
2. **SoundFork Control**의 Broadcast endpoint에서 `CABLE Input`을 선택한다.
3. 게임을 시작하고 **Start relay**를 누른다.
4. Discord에서 애플리케이션 창이 아니라 **전체 모니터 화면**을 공유한다.
5. Discord 마이크는 평소 사용하는 실제 마이크를 그대로 선택한다.

`CABLE Output`을 Discord 마이크로 선택하거나 Windows의 **이 장치로 듣기**를 켤
필요가 없다. 현재 Discord 애플리케이션 창 공유는 별도 릴레이 프로세스의 오디오를
포함하지 않으므로 지원하지 않는다.

## 제거

Windows **설정 → 앱 → 설치된 앱 → SoundFork → 제거** 또는 시작 메뉴의
**Uninstall SoundFork**를 사용한다. 제거 전에 게임과 SoundFork 릴레이를 닫는다.
하드웨어 ASIO 드라이버와 VB-CABLE은 제거되지 않는다.

## 현재 검증 상태

- FiiO 직접 ASIO 대비 체감 지연 차이 없음
- 스테레오 출력, 게임 재시작, 클릭·팝·드롭아웃 없음 확인
- 방송 소비자 중단 시 게임 재생 유지 확인
- Discord 전체 화면 공유 + VB-CABLE 오디오 확인
- Release Setup 설치와 Windows 제거 정상 동작 확인
- Discord 애플리케이션 창 공유는 지원하지 않음
- OBS와 물리 루프백 지연 측정은 아직 미검증

빌드, 구조, 테스트 수치와 릴리스 방법은
[개발자 문서](docs/DEVELOPMENT.md)를 참고한다.

## 라이선스

SoundFork는 GPL-3.0-only로 배포된다. [LICENSE](LICENSE)와
[제3자 구성 요소 고지](THIRD_PARTY_NOTICES.md)를 참고한다.
