# SoundFork 개발 안내

**한국어** | [English](DEVELOPMENT.en.md) · [사용자 안내](../README.md) · [상세 설명](DETAILS.md)

소스를 수정하고 빌드·테스트·배포하려는 개발자를 위한 문서다.
동작 방식, 호환성, 지연 수치와 검증 기록은 [상세 설명](DETAILS.md)에 모은다.

## 개발 환경과 빠른 시작

- Windows 10/11 x64, Git, CMake 3.25 이상
- Visual Studio 2022의 **C++를 사용한 데스크톱 개발** 워크로드와 Windows SDK
- C++20 / MSVC x64 빌드만 지원하며 게임이나 외부 ASIO 드라이버는 자동 테스트에 필요하지 않다.
- Setup.exe 제작에는 Inno Setup 6, 보안 도구 테스트에는 Python 3.10 이상이 추가로 필요하다.

저장소 루트의 PowerShell에서 실행한다. 빌드 스크립트는 Visual Studio에 포함된
CMake도 찾으며, 구성·빌드 후 CTest까지 실행한다.

```powershell
.\scripts\Build-SoundFork.ps1 -Configuration Debug -WithObsPlugin
.\scripts\Build-SoundFork.ps1 -Configuration Release -WithObsPlugin
```

`-WithObsPlugin`을 빼면 OBS 플러그인 없이 빌드한다. 플러그인 포함 빌드는
`Prepare-ObsSdk.ps1`로 공식 OBS 32.2.2 소스·Windows 아카이브의 고정 SHA-256을 검증한다.
동일 버전의 로컬 OBS 런타임은 테스트에 재사용할 수 있다. 공식 헤더와 DLL export로
import library를 생성하며, OBS 런타임 DLL 자체는 배포 패키지에 넣지 않는다.

결과물은 `build/windows-msvc-x64/Debug` 또는 `Release`에 생성된다.
주요 파일은 `soundfork_driver.dll`, `soundfork_control.exe`, `soundfork_capture_hub.exe`,
`soundfork_discord_relay.exe`, 선택적 `soundfork-obs.dll`과 `obs-plugin` 원본 폴더다.
제어판의 플러그인 관리 기능을 시험할 때는 EXE만 따로 옮기지 않는다.

## 소스에서 찾을 위치

- `src/asio-proxy`: x64 ASIO COM 서버와 등록 진입점
- `src/audio-bridge`: 하드웨어 드라이버 로드·생명주기, 버퍼·콜백 중계
- `src/capture-backend`: 공유 링, 캡처 허브, WASAPI 릴레이·클럭 보정, 진단 모니터
- `src/obs-plugin`: 공식 libobs API로 출력하는 오디오 소스와 번역 리소스
- `src/control-app`: 설정·언어·트레이, 업데이트 확인, 권한 분리된 플러그인 관리
- `tests`: 가짜 ASIO 백엔드, 회귀·실제 libobs 통합·보안 도구 테스트
- `installer`, `scripts`, `.github`: 설치 프로그램, 빌드·진단 스크립트, CI·배포 워크플로

버전의 기본값은 `CMakeLists.txt`의 `project(VERSION ...)`이고 사용자 표시 버전은
`SOUNDFORK_VERSION`에서 생성한다. 언어 선택은 사용자별
`HKCU\Software\SoundFork\Language`에 저장한다.

## 수정할 때 지켜야 할 계약

### ASIO 실시간 경로

`ProxyDriver::createBuffers`는 실제 드라이버의 버퍼 포인터를 호스트에 그대로 노출한다.
실제 드라이버의 콜백은 프록시를 거쳐 호스트 콜백을 호출한다. 방송 기능을 위해
재생 버퍼·큐·스레드·한 주기 대기를 끼워 넣지 않는다. 프로세스당 준비된 스트림은
하나이며 두 번째 `createBuffers`는 `ASE_InvalidMode`를 반환한다.

콜백에는 동적 할당, 파일·레지스트리·네트워크 접근, 로깅, 잠금, 대기, 동기식 IPC,
불필요한 샘플 변환이나 방송 소비자 완료 대기를 추가하지 않는다. 캡처 복사를 포함한
10,000콜백의 무할당 테스트를 유지한다. 클럭 보정과 장치 재연결은 방송 경로에만 적용한다.

### 공유 메모리와 캡처 분배

- 원본 링은 `Local\SoundFork.Audio.v2`, OBS·Relay 출력은 각각
  `Local\SoundFork.CaptureHub.v1.OBS`와 `.Relay`다. 각 링은 SPSC 단일 소비자다.
- 최대 8,192프레임 블록 64개와 약 4 MiB 데이터 영역을 시작 전에 준비하고 페이지에 접근한다.
  헤더·매핑 크기·오프셋·포맷 체크섬을 검증하고 복사 크기를 내부에 캐시한다.
- 공유 헤더 값을 콜백의 길이·오프셋으로 다시 신뢰하지 않는다. Interlocked 시퀀스와
  세대 번호의 메모리 순서를 유지하고 재설정 중인 블록을 거부한다.
- 허브가 원본 링의 유일한 소비자가 되어 별도 프로세스에서 두 출력으로 복사한다.
  출력이 가득 차면 해당 출력만 버린다. 진단 모니터와 허브의 동시 원본 소비는 금지한다.
- 이름 있는 뮤텍스로 허브를 단일 실행하고 커널 이벤트 핸들 수명으로 소비자를 추적한다.
  양쪽 소비자 종료 약 2초 뒤 종료하며, 50 ms 간격의 세대·프로세스 확인으로 재시작을 감지한다.
  게임이 없으면 허브 폴링을 50 ms로 낮춘다.

프로토콜을 바꿀 때는 버전·경계 검증과 생산자 재시작 테스트를 함께 갱신한다.
같은 형식으로 재시작한 게임, 강제 종료된 허브, 지연된 한쪽 소비자도 다룬다.

### OBS 출력과 플러그인 관리

OBS 작업 스레드는 480프레임 / 10 ms를 `obs_source_output_audio`에 전달하고 OBS 단조
시계의 연속 타임스탬프를 사용한다. 목표 잔여 큐는 4블록, 보정 한도는 ±2,000 ppm이다.
시작 시 한 패킷과 목표 큐가 준비될 때까지 무음을 보내고, 50 ms 이상 지연되거나 큐가
과도하면 OBS 경로만 재동기화한다. 프로세스 생성·복구는 별도 관리 스레드에 두어
시간을 맞춰 오디오를 전달하는 스레드를 막지 않는다. 종료 통계 로그를 유지한다.

관리 UI는 DLL을 로드하지 않고 버전 리소스를 읽는다. 사용자가 작업을 선택할 때만
같은 제어판 EXE를 `runas`로 실행한다. 고정 관리 명령에 임의의 원본·대상 경로를
받지 않으며 ProgramData Known Folder로 대상을 구한다. OBS 프로세스나 OBS 소비자가
살아 있으면 파일 작업을 거부한다.

링크·재분석 지점을 거부하고 디렉터리 핸들로 경로 교체를 막는다. 알려진 7개 파일만
준비·백업 후 교체·제거하며, 실패하면 이전 변경을 복구한다. 알 수 없는 파일은 보존하고
복구 실패 시 transaction 폴더를 남긴다. 전체 제거도 플러그인 정리에 실패하면 중단한다.
설치 위치·portable 사용법 등 사용자 동작은 [상세 설명](DETAILS.md)에 있다.

## 테스트와 진단

### 자동 회귀와 정적 분석

빌드 스크립트가 CTest를 실행한다. 현재 OBS 포함 시 10개, 제외 시 9개다. 프록시·COM·
공유 링·허브·릴레이 클럭·장치 열거·제어판·플러그인 관리와 선택적 실제 libobs 통합을 다룬다.
libobs 테스트는 DLL 로드, 소스 생성, 좌우 PCM, 타임스탬프 증가와 릴레이 동시 수신을 검증한다.
게임이나 실제 DAC 시험은 아니다.

```powershell
.\scripts\Test-SoundForkStaticAnalysis.ps1 -WithObsPlugin
.\scripts\Test-ReleaseDependencies.ps1
python -B -m unittest discover -s tests -p test_scan_release.py -v
```

정적 분석은 별도 `build/windows-msvc-analyze`에서 `/analyze /WX`로 깨끗이 빌드한다.
Release는 정적 MSVC 런타임을 사용하며 의존성 검사는 x64, CFG·NX·ASLR과 Guard CF
테이블, 동적 MSVC 런타임 미사용을 확인한다. 경고를 억제하기보다 원인을 수정한다.

테스트는 격리된 `SOUNDFORK_CAPTURE_SCOPE`와 임시 경로를 사용한다. 실제 설치된
드라이버·사용자 플러그인·오디오 설정을 테스트 편의를 위해 변경하지 않는다.
디렉터리 심볼릭 링크 시험은 Windows 권한이 없으면 건너뛴다. 실제 링크·정션과
UAC 승인·취소는 별도 수동 시험으로 남기고, 건너뛴 시험을 통과했다고 기록하지 않는다.

### 가속 soak와 하드웨어 시험

```powershell
.\scripts\Test-SoundForkSoak.ps1 -Hours 4
```

이 명령은 48 kHz / 64 samples의 4시간 등가 10,800,000콜백과 ±1,500 ppm 드리프트를
빠르게 시뮬레이션한다. 실제 시간 4시간 재생 시험으로 보고하지 않는다.

실제 시험은 사용자가 릴레이를 시작한 뒤 다음 감시기를 함께 실행한다.

```powershell
.\scripts\Watch-SoundForkHardwareSoak.ps1 -Hours 4
```

콜백 시간·생산자 드롭·큐 깊이·언더런·오버런·메모리를 기록하고, 사람이 좌우 음질,
게임 재시작, Discord 원격 청취·실제 마이크, OBS 녹화·영상 싱크를 확인한다.
소비자 중단과 가상 장치 제거·재연결도 시험하되 기존 설정을 임의로 변경하지 않는다.
지연 보고는 계산값, 드라이버 보고값, 가짜 백엔드 시간, 실제 물리 측정을 분리한다.
시험 결과와 미검증 항목은 [상세 설명](DETAILS.md#검증-기록과-남은-확인)에 갱신한다.

## 개발용 등록과 설치

등록·해제는 시스템 ASIO 목록을 바꾸므로 필요한 경우에만 관리자 권한의 64비트
PowerShell에서 수행한다. 아래 FiiO 이름은 예시이며 다른 드라이버는 실제 등록 이름을 쓴다.

```powershell
.\scripts\Register-SoundFork.ps1 -BackendName 'FiiO ASIO Driver'
.\scripts\Get-SoundForkStatus.ps1 | Format-List
.\scripts\Unregister-SoundFork.ps1
```

ZIP 수동 설치는 압축을 푼 디렉터리의 `scripts/Install-SoundFork.ps1`을 관리자 권한으로
실행하고 `-BackendName`을 지정한다. 제조사 드라이버를 삭제·수정하지 않는다.
설치·COM 등록·제거 자동 시험은 격리된 CI runner에서 임시 ASIO 백엔드로 수행한다.
`Test-ReleaseInstaller.ps1 -AllowRegistryChanges`는 실제 레지스트리를 바꾸며 기존
SoundFork 키를 발견하면 중단한다. 사용 중인 PC에서 무심코 실행하지 않는다.

## 패키징과 릴리스

### 로컬 패키지

Inno Setup 6이 있으면 Setup.exe와 ZIP, `SHA256SUMS.txt`를 `artifacts`에 만든다.
아래 버전은 로컬 패키징 예시이며 이미 존재하는 태그를 재생성하라는 뜻이 아니다.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-beta.2
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-beta.2 -SkipInstaller
```

인증서가 있으면 모든 배포 EXE·DLL과 Setup을 SHA-256 / RFC 3161로 서명·검증한다.
인증서 비밀키·비밀번호는 저장소에 넣지 않는다.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-beta.2 `
  -CertificateThumbprint '<SHA1 thumbprint>' `
  -TimestampUrl 'https://timestamp-provider.example'
```

아이콘은 `assets/soundfork-icon.png`를 수정한 뒤 `scripts/Build-Icon.ps1`로 다시 만든다.
문서를 추가·이동하면 CMake 설치 목록과 README 링크를 함께 갱신한다.
ZIP과 설치본 모두 README는 루트, 상세·개발 문서는 `docs`에 두는 상대 경로를 유지한다.

### GitHub 배포 절차

1. Debug·Release 회귀와 정적 분석을 확인하고 변경 사항을 커밋한다.
2. 미사용 버전을 정한다. 기본 `X.Y.Z`는 CMake와 일치해야 하며 선택 접미사는
   `alpha.N`, `beta.N`, `rc.N`이다. 한·영 README의 Setup 링크도 같은 버전으로 맞춘다.
3. 배포 커밋을 원격 `main`에 반영한 뒤 그 커밋에 태그를 붙여 푸시한다.
   아래 명령은 배포 담당자가 의도적으로 실행할 절차이며 기존 태그를 강제로 옮기지 않는다.

```powershell
$releaseVersion = Read-Host 'New release version without v'
git tag -a "v$releaseVersion" -m "SoundFork v$releaseVersion"
git push origin "v$releaseVersion"
```

PR·`main` CI는 Windows Debug·Release 빌드·CTest, Release 정적 분석과 보안 도구 테스트를
실행한다. 태그 릴리스는 **Windows 빌드·기존 검증 → ClamAV → 게시** 순서다.
서명이 있으면 검사 전에 완료하고, 검사·게시 단계는 같은 불변 아티팩트 ID와 빌드가
기록한 체크섬 목록 SHA-256으로 파일을 재검증한다. 재빌드·재서명하지 않는다.
게시 전에 태그 이동을 거부하고 같은 태그의 실행은 직렬화한다. `contents: write`는
게시 작업에만 주며, 검사 실패 시 이미 게시된 과거 릴리스를 삭제하지 않는다.

`WINDOWS_SIGNING_PFX_BASE64`, `WINDOWS_SIGNING_PFX_PASSWORD`,
`WINDOWS_TIMESTAMP_URL`을 모두 설정하면 서명한다. 없으면 미서명 경고를 내며
접미사 태그는 prerelease, 접미사 없는 태그는 정식 Release로 게시한다.

## 보안 검사 운영

### CodeQL과 의존성

CodeQL은 `main` push·PR·매주 화요일 03:23 UTC·수동 실행에서 OBS 포함 Windows
C/C++ 빌드를 직접 추적하고 `security-extended` 결과를 Security → Code scanning에
등록한다. 기존 CI·배포와 독립적이며 분석 실행 성공이 경고 0개라는 의미는 아니다.
PowerShell·Inno Setup·Python이나 실제 장치 동작은 이 C/C++ 분석의 대상이 아니다.
공개 저장소의 Code scanning·Actions가 필요하며 Default setup을 쓰고 있었다면
중복되지 않도록 Advanced setup으로 전환한다. 설정을 자동 변경하지 않는다.

[Dependabot 설정](../.github/dependabot.yml)은 Actions 의존성을 주 1회 확인한다.
전체 커밋 SHA 고정과 수동 PR 검토를 유지하고 자동 병합하지 않는다. CMake나 수동
다운로드 SDK는 이 설정의 대상이 아니며 OBS 아카이브의 SHA-256 검증을 유지한다.

### ClamAV와 실패 대응

Ubuntu ClamAV 패키지와 `freshclam` 공식 정의로 최종 Setup.exe·ZIP·안전하게 추출한
ZIP 내용을 실행하지 않고 검사한다. `scripts/scan_release.py`가 파일·체크섬 목록과
빌드 기록의 해시를 확인한다. ZIP 경로 이탈·링크·암호화·중복 경로도 거부한다.

- 원본·추출 단일 파일 128 MiB, ZIP 추출 합계·ClamAV 컨테이너 확장 크기 512 MiB
- ZIP 항목·컨테이너 파일 수 4,096개, 깊이 32
- 파일별 120초, 스캐너 프로세스 15분, 정의 최대 나이 3일

탐지·제한 초과·경고·오류·시간 초과·정의 갱신 실패는 게시를 막는다. 자동 예외 처리,
검사 우회나 탐지 파일 삭제는 하지 않는다. Actions 로그와 30일 보관하는
`SoundFork-<version>-security-logs`의 `freshclam.log`, `clamscan.log`,
`scan-result.json`을 확인한다. 도달한 단계의 로그만 존재하며 엔진·정의 버전과 대상
SHA-256을 기록한다. 오탐이 의심돼도 원인을 검토한 뒤 수정·재실행한다.

Python 테스트는 표준 라이브러리만 사용하며 일반 CI·로컬에서는 성공·탐지·오류·시간
초과를 모의 실행한다. 릴리스 Linux 작업만 `SOUNDFORK_TEST_CLAMAV=1`로 실제 엔진과
임시 EICAR 파일을 시험한다. 샘플은 커밋·배포하지 않는다. 실제 업로드·정의 갱신·게시
차단 여부는 GitHub Actions 실행 기록으로 검증하고 로컬 시험과 구분한다.
보안 검사의 의미와 한계는 [상세 설명](DETAILS.md#보안-검사와-신뢰-범위)에 있다.

## 라이선스와 기여

GPL-3.0-only와 [제3자 구성 요소 고지](../THIRD_PARTY_NOTICES.md)를 확인하고
재배포할 수 없는 SDK·런타임·인증서 자료를 추가하지 않는다. 재생 경로 변경과 방송·UI
변경을 분리해 검토하고, 검증 범위와 미실시 시험을 PR에 명시한다.
