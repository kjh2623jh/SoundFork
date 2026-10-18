# SoundFork 개발자 문서

**한국어** | [English](DEVELOPMENT.en.md) · [사용자 안내](../README.md)

이 문서는 SoundFork의 구조, 실시간 처리 원칙, 빌드·테스트 방법과 현재 검증 범위를
설명한다. 일반적인 설치와 Discord 사용법은 [README](../README.md)를 참고한다.

## 구성 요소

- `asio-proxy`: ASIO 호스트에 `SoundFork`로 노출되는 64비트 COM DLL
- `audio-bridge`: 실제 하드웨어 ASIO 드라이버의 생명주기와 버퍼를 중계
- `capture-backend`: 비차단 공유 링, 진단 모니터, WASAPI 방송 릴레이
- `control-app`: 백엔드 선택, 상태 확인, 릴레이 시작·중지
- `tests`: 가짜 ASIO 드라이버, 프록시·COM·공유 링 자동 테스트
- `installer`: Inno Setup 기반 설치 프로그램

제어판은 저장된 사용자 선택이 없으면 Windows UI가 한국어일 때만 한국어를 사용하고
그 밖의 언어에서는 영어를 사용한다. 설치 프로그램이 선택한 언어는 최초 값으로만
전달되며, 이후 **Language/언어** 메뉴에서 바꾼 값은
`HKCU\Software\SoundFork\Language`에 유지된다.

SoundFork는 DJMAX 전용 후킹 프로그램이 아니다. 표준 64비트 ASIO COM 인터페이스를
사용하는 다른 게임과 DAW에서도 선택할 수 있다. 현재 실제 하드웨어 검증 환경만
DJMAX RESPECT V와 FiiO K11 R2R이다.

## 재생과 캡처 구조

```text
64비트 ASIO 호스트
  -> soundfork_driver.dll
      +-> 하드웨어 ASIO 드라이버 -> DAC
      +-> 공유 메모리 SPSC 링 -> soundfork_discord_relay.exe
                                  -> VB-CABLE -> Discord 전체 화면 공유
```

`createBuffers`는 하위 드라이버가 제공한 버퍼 포인터를 호스트에 그대로 노출한다.
게임은 하드웨어 드라이버 소유 버퍼에 직접 쓰므로 재생 경로에 추가 버퍼나 샘플
변환이 없다. ASIO 콜백에는 컨텍스트 포인터가 없으므로 프로세스당 준비된 스트림
하나만 허용하며, 두 번째 `createBuffers`는 `ASE_InvalidMode`를 반환한다.

48 kHz 스테레오 `ASIOSTInt32LSB`에서는 완료된 출력 블록을
`Local\SoundFork.Audio.v1` 공유 메모리로 인터리브 복사한다. 링은 최대 8,192
프레임 블록 64개와 소비자 하나를 지원한다. 소비자가 없거나 늦으면 캡처 블록만
버리고 `dropped_blocks`를 증가시킨다. DAC 출력은 기다리지 않는다.

### 실시간 콜백 규칙

콜백에서는 다음 작업을 하지 않는다.

- 동적 메모리 할당
- 파일, 레지스트리, 네트워크 접근이나 로깅
- 잠금, 대기 또는 블로킹 동기화
- 불필요한 샘플 변환
- 방송 소비자의 처리 완료 대기

자동 테스트는 캡처 발행을 포함한 콜백 10,000회에서 동적 할당이 0회인지 검증한다.

## Discord 동작 경계

ASIO는 Windows 공유 오디오 엔진을 우회한다. 직접 FiiO ASIO로 보낸 오디오는 일반
WASAPI 루프백에 나타나지 않는다. 릴레이가 만드는 Windows 오디오 스트림의 소유자는
DJMAX가 아니라 `soundfork_discord_relay.exe`다.

실제 원격 청취 테스트 결과는 다음과 같다.

- DJMAX 애플리케이션 창 공유: 릴레이를 사용해도 게임 오디오가 포함되지 않음
- 전체 화면 공유 + 직접 ASIO: 게임 오디오 없음
- 전체 화면 공유 + SoundFork 릴레이 + VB-CABLE: 게임 오디오 포함

따라서 현재 지원 방식은 전체 화면 공유다. Windows 11의
`IAudioViewManagerService`를 이용한 HWND 연결도 시험했지만 Discord 창 공유에는
외부 릴레이 스트림이 포함되지 않았다. Discord 코드 주입이나 비공개 API는 사용하지
않는다.

관련 공개 문서:

- <https://support.discord.com/hc/en-us/articles/360040816151-Go-Live-and-Screen-Share>
- <https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/>
- <https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioviewmanagerservice>

## 빌드

필요한 환경은 Windows 10/11 x64, Visual Studio 2022의 **C++를 사용한 데스크톱
개발** 워크로드와 CMake 3.25 이상이다.

```powershell
.\scripts\Build-SoundFork.ps1 -Configuration Debug
.\scripts\Build-SoundFork.ps1 -Configuration Release
```

Release 바이너리는 정적 MSVC 런타임을 사용한다. 주요 결과물은
`build/windows-msvc-x64/<Config>` 아래에 생성된다.

앱 아이콘 원본은 `assets/soundfork-icon.png`이며 다중 크기 Windows 아이콘은
다음 명령으로 다시 만들 수 있다.

```powershell
.\scripts\Build-Icon.ps1
```

설치 프로그램을 포함한 로컬 배포 패키지는 Inno Setup 6이 필요하다.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-alpha.1

# Inno Setup 없이 ZIP만 검사
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-alpha.1 -SkipInstaller
```

## 테스트

CTest는 프록시 코어, COM 서버, ASIO 호스트 열거, 공유 링, WASAPI 엔드포인트 열거,
컨트롤 앱 자체 검사 등 6개 테스트를 실행한다. 2026-10-19 기준 Debug와 Release에서
각각 6/6 통과했다.

하드웨어 테스트에서는 FiiO ASIO Driver 6.16.0.0과 FiiO K11 R2R를 48 kHz / 64
samples / 스테레오 `ASIOSTInt32LSB`로 실행했다. 직접 경로와 프록시 경로 모두
168 samples의 ASIO 출력 지연을 보고했고, 사용자가 좌우 재생, 체감 지연 차이 없음,
클릭·팝·드롭아웃 없음과 게임 재시작을 확인했다. 168 samples는 드라이버 보고값이며
물리 루프백 측정값은 아니다.

공유 링은 늦은 소비자와 소비자 중단 상황에서도 FiiO 콜백이 계속되는지 검증했다.
등록된 COM 경로의 3초 소비 구간에서는 2,244블록 / 143,616프레임, 생산자 드롭
0개를 기록했다. 2026-10-19에는 GitHub Release Setup을 실제 Windows 환경에
설치하고 Windows 제거 기능으로 정상 제거되는 것도 확인했다.

### 아직 주장하지 않는 항목

- 공유 메모리 모니터에서 0이 아닌 DJMAX 곡 PCM 캡처 확인
- 수 시간 규모의 캡처 soak 테스트
- Discord 애플리케이션 창 공유 오디오
- 실제 마이크와 전체 화면 릴레이의 동시 원격 확인
- OBS 연동
- 물리 루프백 장비를 사용한 실제 지연 측정

## 등록과 설치

개발용 수동 등록은 관리자 권한의 64비트 PowerShell에서만 수행한다.

```powershell
.\scripts\Register-SoundFork.ps1 -BackendName 'FiiO ASIO Driver'
.\scripts\Get-SoundForkStatus.ps1 | Format-List
.\scripts\Unregister-SoundFork.ps1
```

설치 프로그램은 등록된 x64 ASIO 백엔드를 선택받고, 자신과 같은 폴더의
`soundfork_driver.dll`만 등록한다. 백엔드 설정에 실패하면 SoundFork 등록을
롤백한다. 제거 과정은 다른 ASIO 드라이버나 가상 오디오 장치를 건드리지 않는다.

## CI/CD

PR과 `main` push는 Windows x64 Debug/Release 빌드와 CTest를 실행한다. 다음 형식의
태그는 Setup.exe, 실행 가능 ZIP과 SHA-256 파일을 생성한다.

```powershell
git tag -a v0.1.0-alpha.1 -m 'SoundFork v0.1.0-alpha.1'
git push origin v0.1.0-alpha.1
```

지원되는 접미사는 `alpha.N`, `beta.N`, `rc.N`이며 GitHub prerelease로 게시된다.
정식 `vX.Y.Z` 태그는 일반 Release가 된다. 태그 기본 버전은 `CMakeLists.txt`와
일치하고 태그 커밋은 `main`에 포함되어야 한다. 새 버전을 태그하기 전에 사용자
README 두 개의 Setup 바로 다운로드 링크도 같은 태그로 갱신해야 한다.

Release workflow는 x64 PE와 런타임 의존성, 체크섬, 임시 ASIO 백엔드를 사용한
설치·COM 등록·제거를 검증한 뒤에만 게시한다. 설치 테스트는 기존 SoundFork 키를
발견하면 덮어쓰지 않고 중단한다. 실제 하드웨어와 Discord 원격 청취는 CI에서
재현하지 않는다.

## 라이선스

SoundFork는 GPL-3.0-only다. ASIO SDK, 선택적 VB-Audio 소프트웨어 및 Inno Setup
관련 내용은 [제3자 구성 요소 고지](../THIRD_PARTY_NOTICES.md)를 참고한다.
