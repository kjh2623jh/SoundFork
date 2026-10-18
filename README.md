# SoundFork

**한국어** | [English](README.en.md)

> 이 프로젝트는 AI의 도움을 받아 개발되었습니다. 아키텍처 결정, 코드 검토,
> 실제 하드웨어 및 Discord 검증은 프로젝트 관리자가 수행했습니다.

SoundFork는 Windows x64용 실험적 사용자 모드 ASIO 프록시다. ASIO 호스트가
`soundfork_driver.dll`을 불러오면 SoundFork가 설정된 하드웨어 ASIO 드라이버를
열고, 하드웨어 드라이버의 더블 버퍼 주소를 호스트에 직접 노출한다. 재생 경로에는
추가 버퍼나 샘플 변환이 없다. 48 kHz 스테레오 `ASIOSTInt32LSB` 출력은 외부 캡처
소비자가 읽을 수 있도록 비차단 공유 메모리 SPSC 링에도 복제된다.

현재 기준 환경은 DJMAX RESPECT V, 48 kHz / 64 samples, FiiO ASIO Driver다.
실제 장치에서 프록시 경로와 직접 ASIO 경로를 모두 시험했으며, 스테레오 재생,
안정성, 게임 재시작, 체감 지연 차이 없음이 확인됐다. Discord 전체 화면 공유는
VB-CABLE을 통한 릴레이로 소리 전달을 확인했다. Discord 애플리케이션 창 공유에는
릴레이 오디오가 포함되지 않았다. OBS 연동은 아직 검증하지 않았다.

자세한 문서:

- [아키텍처](ARCHITECTURE.md)
- [테스트 및 검증 결과](TESTING.md)
- [Discord 연동 상태](DISCORD.md)
- [제3자 구성 요소 고지](THIRD_PARTY_NOTICES.md)

## 빌드

필요한 환경:

- Windows 10/11 x64
- Visual Studio 2022의 **C++를 사용한 데스크톱 개발** 워크로드
- CMake 3.25 이상(Visual Studio 내장 CMake 사용 가능)

PowerShell에서 실행한다.

```powershell
.\scripts\Build-SoundFork.ps1 -Configuration Debug
.\scripts\Build-SoundFork.ps1 -Configuration Release
```

주요 결과물:

- `build/windows-msvc-x64/<Config>/soundfork_driver.dll`
- `build/windows-msvc-x64/<Config>/soundfork_control.exe`
- `build/windows-msvc-x64/<Config>/soundfork_host.exe`
- `build/windows-msvc-x64/<Config>/soundfork_tests.exe`

## 구성 요소

- `soundfork_driver.dll`: 게임이나 DAW가 불러오는 ASIO 프록시 DLL이다. 독립
  실행 프로그램이 아니므로 작업 관리자에 별도 프로세스로 표시되지 않는다.
- `soundfork_discord_relay.exe`: 공유 메모리의 복제 오디오를 가상 WASAPI 출력으로
  보내는 프로세스다. 릴레이가 실행 중일 때만 작업 관리자에 나타난다.
- `soundfork_control.exe`: ASIO 백엔드 선택, 상태 확인, 릴레이 시작·중지를 제공하는
  선택적 Win32 컨트롤 앱이다. 이 앱을 닫아도 이미 실행 중인 릴레이는 계속 동작한다.

## 설치, 상태 확인 및 제거

### GitHub Release 설치 프로그램

일반 사용자는 GitHub Releases에서 `SoundFork-Setup-vX.Y.Z.exe`를 내려받아
실행한다. 설치 프로그램이 등록된 x64 하드웨어 ASIO 드라이버를 보여주며,
FiiO ASIO Driver가 있으면 기본 선택한다. 설치가 끝나면 시작 메뉴의
**SoundFork Control**에서 상태와 방송 엔드포인트를 확인할 수 있다.

초기 v0.x 배포 파일은 코드 서명되지 않은 Alpha이므로 Windows SmartScreen 경고가
표시될 수 있다. 다운로드한 파일의 SHA-256은 같은 Release의 `SHA256SUMS.txt`와
비교할 수 있다. SoundFork 설치 프로그램은 VB-CABLE, VoiceMeeter 또는 하드웨어
ASIO 드라이버를 포함하거나 제거하지 않는다.

고급 사용자용 `SoundFork-vX.Y.Z-windows-x64.zip`에는 실행 파일, 수동 설치·제거
스크립트, 한국어·영어 문서가 들어 있다. GitHub가 자동으로 제공하는 “Source code”
ZIP은 실행 가능한 Windows 배포판이 아니다.

### 소스 트리에서 설치

개발용 등록은 빌드 폴더의 DLL을 Windows 레지스트리에 직접 연결한다. 빌드 폴더를
옮기거나 삭제하면 등록이 깨지므로 안정적인 설치에는 DJMAX와 릴레이를 닫고
**64비트 PowerShell을 관리자 권한으로 실행**한 뒤 다음을 사용한다.

```powershell
.\scripts\Install-SoundFork.ps1
```

Release 바이너리를 `C:\Program Files\SoundFork`에 복사하고 SoundFork만 등록한 뒤
기본 백엔드를 `FiiO ASIO Driver`로 지정한다. FiiO나 다른 ASIO 드라이버를 삭제하거나
수정하지 않는다.

상태 확인에는 관리자 권한이 필요 없다.

```powershell
.\scripts\Get-SoundForkStatus.ps1 | Format-List
```

컨트롤 앱 실행:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_control.exe
```

컨트롤 앱 자체는 일반 사용자 권한으로 실행된다. 하드웨어 ASIO 백엔드를 바꾸고
**Apply**를 누를 때만 레지스트리 변경을 위한 UAC가 표시된다. 변경은 ASIO 호스트를
다시 실행할 때 적용된다.

방송 엔드포인트는 설치된 기본 VB-CABLE을 우선하고, 없으면 VoiceMeeter Desktop을
선택한다. 저장된 유효한 선택은 유지하며 물리 스피커나 DAC를 자동 선택하거나
Windows 기본 장치를 변경하지 않는다. 지원되는 가상 엔드포인트가 없으면
**Get VB-CABLE** 버튼이 공식 다운로드 페이지를 연다.

제거하려면 DJMAX와 릴레이를 닫고 관리자 PowerShell에서 실행한다.

```powershell
.\scripts\Remove-SoundFork.ps1
```

SoundFork 등록과 `C:\Program Files\SoundFork` 아래의 알려진 파일만 제거한다. FiiO,
VB-CABLE 또는 VoiceMeeter는 제거하지 않는다. 개발용 `Register-SoundFork.ps1`과
`Unregister-SoundFork.ps1`도 별도로 사용할 수 있다.

ASIO 호스트가 SoundFork의 ASIO 컨트롤 패널 메서드를 호출하면 현재는 선택된
하드웨어 백엔드의 패널을 연다. 별도 SoundFork 컨트롤 앱은 재생에 필수 요소가 아니다.

## Discord 전체 화면 공유

검증된 경로는 다음과 같다.

```text
DJMAX
  -> SoundFork ASIO
      +-> FiiO ASIO -> DAC                  게임 재생 경로
      +-> 공유 메모리 -> 릴레이 -> VB-CABLE 방송 복제 경로
```

릴레이 시작과 중지:

```powershell
.\scripts\Start-SoundForkRelay.ps1
.\scripts\Stop-SoundForkRelay.ps1
```

DJMAX에서 `SoundFork`를 선택하고 Discord에서는 **전체 모니터**를 공유한다. 스크립트는
VB-CABLE을 우선 탐색하고 없으면 VoiceMeeter 엔드포인트를 사용한다. 가상 엔드포인트는
비동기 방송 복제에만 사용되므로 하드웨어 ASIO 재생 경로에 들어가지 않는다.

VB-CABLE은 SoundFork에 포함되지 않는 별도 donationware 제품이다. 공식 페이지
<https://vb-audio.com/Cable/>에서 내려받아 관리자 권한으로 설치하고, 공급사 안내대로
재부팅한다. Discord 마이크는 실제 마이크를 그대로 선택한다. `CABLE Output`을
Discord 마이크로 선택하거나 Windows의 **이 장치로 듣기**를 켤 필요가 없다.

Discord 애플리케이션 창 공유는 지원되지 않는다. 릴레이 WASAPI 스트림의 소유자는
DJMAX가 아니라 `soundfork_discord_relay.exe`이며, 실제 Discord 테스트에서 DJMAX 창
공유가 이 외부 스트림을 포함하지 않았다. 공개된 Discord PCM 삽입 API도 없다.
자세한 내용은 [Discord 연동 상태](DISCORD.md)를 참고한다.

## 안전한 테스트 순서

자동 테스트는 레지스트리를 변경하거나 FiiO 장치를 열지 않는다.

```powershell
& .\build\windows-msvc-x64\Debug\soundfork_tests.exe
& .\build\windows-msvc-x64\Debug\soundfork_host.exe --list
```

SoundFork를 설치하지 않고 실제 백엔드를 감싸는 조회 테스트:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_host.exe `
  --proxy-backend 'FiiO ASIO Driver'
```

실제 스트림 테스트는 하드웨어 드라이버를 연다.

```powershell
& .\build\windows-msvc-x64\Release\soundfork_host.exe `
  --proxy-backend 'FiiO ASIO Driver' --sample-rate 48000 --buffer-size 64 `
  --channels 0,1 --duration 10 --signal silence
```

좌우 채널을 확인할 때는 먼저 실제 출력 볼륨을 낮춘다. 테스트 톤 기본값은
-40 dBFS다.

```powershell
& .\build\windows-msvc-x64\Release\soundfork_host.exe `
  --proxy-backend 'FiiO ASIO Driver' --sample-rate 48000 --buffer-size 64 `
  --channels 0,1 --duration 2 --signal sine-left --gain-db -40

& .\build\windows-msvc-x64\Release\soundfork_host.exe `
  --proxy-backend 'FiiO ASIO Driver' --sample-rate 48000 --buffer-size 64 `
  --channels 0,1 --duration 2 --signal sine-right --gain-db -40
```

신호 경로와 볼륨을 확인하기 전에는 더 큰 `--gain-db` 값을 사용하지 않는다.

## 공유 메모리 캡처 모니터

Phase 3 진단 소비자는 SoundFork 스트림 실행 전이나 실행 중에 시작할 수 있다.

```powershell
& .\build\windows-msvc-x64\Release\soundfork_capture_monitor.exe `
  --wait 30 --duration 10
```

협상된 포맷, 소비 블록과 프레임 수, PCM 피크, 생산자 드롭 수를 출력한다. 오디오를
재생하거나 파일로 저장하지 않는다. 링에는 한 번에 한 소비자만 연결할 수 있다.
소비자가 늦거나 중단되어 링이 가득 차더라도 ASIO 콜백은 기다리지 않고 캡처 블록만
버리므로 게임 재생은 계속된다.

## 라이선스

SoundFork는 GPL-3.0-only로 배포된다. `LICENSE`와
[제3자 구성 요소 고지](THIRD_PARTY_NOTICES.md)를 참고한다.

## CI/CD와 릴리스

PR과 `main` push는 GitHub Actions에서 Windows x64 Debug/Release 빌드와 CTest를
실행한다. 태그의 기본 버전은 `CMakeLists.txt`의 `project(... VERSION ...)`와 같아야
한다.

```powershell
# CMake VERSION 0.1.0의 사전 릴리스
git tag -a v0.1.0-alpha.1 -m 'SoundFork v0.1.0-alpha.1'
git push origin v0.1.0-alpha.1

# 정식 릴리스
git tag -a v0.1.0 -m 'SoundFork v0.1.0'
git push origin v0.1.0
```

`alpha`, `beta`, `rc` 태그는 GitHub prerelease로 게시된다. Release workflow는
Setup.exe, Windows x64 ZIP, SHA-256 파일을 만들고 설치·제거 smoke test가 통과한
경우에만 업로드한다. 로컬 패키지 생성은 다음 명령을 사용한다.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-alpha.1
```

Setup.exe 생성에는 Inno Setup 6이 필요하다. ZIP만 확인하려면
`-SkipInstaller`를 사용할 수 있다.
