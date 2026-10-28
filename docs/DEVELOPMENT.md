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
CMake도 찾으며, 구성·빌드 후 기본적으로 CTest까지 실행한다. `-SkipTests`는 컴파일은 유지하고
CTest 실행만 생략하는 CodeQL용 옵션이며 일반 CI·릴리스에서는 사용하지 않는다.
현재 브랜치의 기본 빌드는 `1.1.0-dev`다. 개발 빌드에서는 `-Version`을 생략한다.
아래 패키징 예시의 alpha 번호는 명시적인 시험판 준비 시에만 사용하며 자동 배포하지 않는다.

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

- 원본 링은 `Local\SoundFork.Audio.v3`, OBS·Relay 출력은 각각
  `Local\SoundFork.CaptureHub.v2.OBS`와 `.Relay`다. 각 링은 SPSC 단일 소비자다.
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
시계의 연속 타임스탬프를 사용한다. 타이머 기상 횟수가 아닌 누적 샘플 기한으로
one-shot 타이머를 재설정하고, 늦게 기상하면 최대 5패킷까지 따라잡는다. 50 ms를
초과한 정체나 과도한 큐는 OBS 경로만 재동기화한다. 전송 스레드는 MMCSS `Audio`에
등록하되 실패하면 경고 후 계속한다. 프로세스 생성·복구는 별도 관리 스레드에 둔다.

OBS 목표 잔여 큐는 최소 20 ms와 4블록 중 큰 값으로, 64 samples에서는 960프레임이다.
한 패킷과 보간 look-ahead까지 준비한 뒤 시작하며 실제 언더런 후에도 재충전한다.
보정 한도는 ±2,000 ppm이고, 따라잡을 패킷은 드리프트 판단용 큐에서 제외한다.
시작 여유량이 48블록 상한보다 작은 포맷만 받는다(48 kHz / 64 samples 지원).
이는 방송용 버퍼 목표이지 측정된 총 지연이 아니며 FiiO 재생과 WASAPI Relay에는 적용하지 않는다.

짧은 입력 부족에는 타임스탬프를 유지한 채 최대 20 ms 동안 one-shot 타이머로 1 ms마다
재확인한다. 즉시 무음을 넣지 않고 늦게 도착한 패킷을 따라잡으며, 시간이 초과되면
언더런·재충전 절차로 전환한다. ASIO 생산자를 대기시키거나 busy-spin하지 않는다.
이 재시도는 방송 패킷의 일시적인 도착 지연을 허용하는 것이며 지연 0 보장이 아니다.

OBS 로그에 연결·연결 해제와 30초 주기/종료 누적 통계를 남긴다. `idle_silence_frames`는
생산자 대기, `prebuffer_silence_frames`는 초기/복구 충전, `underrun_silence_frames`는
재생 중 데이터 부족이다. `deadline_resyncs`와 `queue_resyncs`를 구분하며
`discarded_blocks`는 링에서 폐기한 블록 수다. `hub_output_drops`는 OBS 출력 링의
드롭만 뜻한다. 누적값을 특정 녹화 한 번의 값으로 해석하지 않는다.
`input_wait_wakes`는 짧은 입력 부족에 대한 재확인 횟수이며 오디오 드롭 수가 아니다.
`max_lateness_us`는 입력 재시도를 포함한 패킷 기한 대비 지연이며 ASIO 콜백 시간이 아니다.

관리 UI는 DLL을 로드하지 않고 버전 리소스를 읽는다. 사용자가 작업을 선택할 때만
같은 제어판 EXE를 `runas`로 실행한다. 고정 관리 명령에 임의의 원본·대상 경로를
받지 않으며 ProgramData Known Folder로 대상을 구한다. OBS 프로세스나 OBS 소비자가
살아 있으면 파일 작업을 거부한다.

링크·재분석 지점을 거부하고 디렉터리 핸들로 경로 교체를 막는다. 알려진 7개 파일만
준비·백업 후 교체·제거하며, 실패하면 이전 변경을 복구한다. 알 수 없는 파일은 보존하고
복구 실패 시 transaction 폴더를 남긴다. 전체 제거도 플러그인 정리에 실패하면 중단한다.
설치 위치·portable 사용법 등 사용자 동작은 [상세 설명](DETAILS.md)에 있다.

### 사용자 설정과 자동 시작

`control-app/relay_settings.*`는 `HKCU\Software\SoundFork\RelayEndpointId`와
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`의 `SoundFork` 값을 관리한다.
Run 값 자체가 자동 시작 설정이며, 인용한 절대 `soundfork_control.exe` 경로와
`--startup`만 등록한다. 260자를 넘는 명령과 잘못된 REG_SZ는 거부한다.
다른 프로그램의 같은 이름 값은 덮어쓰지 않으며 해제·제거는 현재 EXE와 정확히
일치하는 등록만 지운다. Windows의 StartupApproved 내부 값은 읽거나 변경하지 않는다.

`--startup`은 등록이 없으면 조용히 종료하고, 등록이 있으면 트레이와 persistent 릴레이를
시작한다. 제어판은 세션 로컬 뮤텍스로 단일 실행하며 일반 재실행은 기존 창을 표시한다.
`--self-test`, 플러그인 관리와 `--remove-user-startup`은 GUI 뮤텍스를 사용하지 않는다.
마지막 명령은 제거 시 현재 계정의 현재 EXE 등록만 정리한다. 다른 계정 프로필은 건드리지 않는다.
ZIP 제거 스크립트는 버전 리소스의 `SoundFork.StartupManagement/1` capability를 확인한다.

UI는 릴레이에 `--endpoint-id <opaque-id>`를 전달한다. 정확한 ID가 없으면 재연결을
기다리며 이름 부분 일치나 기본 출력 fallback을 하지 않는다. 기존 수동 CLI의
`--endpoint <name-part>` 검색은 유지한다. 방송 오디오 처리·클럭 보정과 ASIO 경로는 바꾸지 않는다.

## 테스트와 진단

### 자동 회귀와 정적 분석

빌드 스크립트가 기본적으로 CTest를 실행한다. 현재 OBS 포함 시 27개, 제외 시 20개다. 프록시·COM·
공유 링·허브·릴레이 클럭·장치 열거·제어판·플러그인 관리와 선택적 실제 libobs 통합을 다룬다.
libobs 테스트는 DLL 로드, 소스 생성, 연속 스테레오 파형, 타임스탬프 간격, 예기치 않은
무음·불연속, 생산자 일시 중단 후 복구와 릴레이 동시 수신을 검증한다. 생산자는 누적
시간 기준으로 샘플을 공급한다. 별도 가상 시계 테스트는 지연/누락 기상과 긴 정체,
4시간 등가 지터·±1,500 ppm 드리프트를 다룬다. 실제 게임이나 DAC 시험은 아니다.
테스트 생산자도 MMCSS `Audio`를 사용하고 최대 생산 간격·허브 공급 관측 간격을 기록한다.
30 ms 분량의 PCM 전송을 일부러 늦춘 구간도 무음·파형·타임스탬프 검사를 제외하지 않는다.

### 검증 범위와 실행 비용

| 구분 | 자동으로 확인하는 것 | 아직 확인하지 못하는 것 |
| --- | --- | --- |
| 프록시·변환기·링 | 버퍼 포인터·콜백·게시 횟수, 네 형식의 원본 바이트, 경계값·헤더 변조, C++ `new/new[]` 감시 | 실제 제조사 ASIO 동작·물리 지연, 모든 CRT/Win32 할당을 망라한 증명 |
| 허브·가속 soak | 동시 분배·느린 소비자 격리·재시작·충돌 복구, 반복 PCM·카운터·큐·메모리 관측 | 실제 시간 경과·하드웨어 클럭·WASAPI 장치 장애, 메모리 보고만으로 모든 누수 부재 증명 |
| libobs·가상 시계 | 실제 libobs가 받은 네 형식 파형·좌우·PTS·무음·지터, 정수/float 복구·가상 드리프트 | 실제 OBS 영상·인코더·Discord 원격 청취·DAC와의 물리 싱크 |
| 제어판·설정 | 숨겨진 실제 Win32 UI, 임시 HKCU 설정·권한·단일 실행·자동 시작 조기 종료 | 실제 로그인·UAC·클립보드·모든 DPI와 사용자 장치 환경 |
| 보안 검사 | CodeQL·MSVC 분석, 스캐너 실패·경로·체크섬·게시 정책, 릴리스 ClamAV 관문 | 취약점/악성코드 부재 보장·코드 서명·일반 CTest만으로 실제 백신 검사 |
| 배포 검사 | CTest의 설치 선택 정책·프로세스 시간 제한·DLL 인터페이스, 격리 릴리스 CI의 설치·업데이트·제거 | 사용 중인 PC에서의 실제 설치·관리자 승인; 로컬 정책 시험은 설치 완료 증거가 아님 |

Debug/Release, MSVC 정적 분석, CodeQL은 목적이 달라 유지한다. CodeQL은 테스트 소스도
컴파일하되 실행은 일반 CI/릴리스에 맡긴다. 릴리스 자체의 전체 CTest도 최종 버전의 빌드와
배포 파일을 연결하는 관문이므로 유지한다. 보안 도구 정책 시험은 설정 텍스트를 검사하며
GitHub Actions의 실제 실행을 대신하지 않는다. CI Windows 작업은 30분 제한이다.

실제 libobs의 형식별 시험은 약 8초씩 유지한다. 시작 안정화 1.5초, 4초 지점의 지연 주입,
48,000프레임 이상 연속 파형 검사가 있어 단축하면 검증 범위를 잃는다. 정수/float 복구는
서로 다른 렌더러를 검사한다. 설정·단일 실행·비활성 시작의 분리는 짧고 실패 위치를 구분한다.
허브 형식 시험만 한 프로세스에서 네 형식으로 재연결해, 반복적인 약 2초 종료 대기를 없앴다.
별도의 느린 소비자·강제 종료·자동 종료 시험은 유지한다. 빌드 스크립트는 모든 기본 타깃을
한 번 빌드하므로 이후 같은 세 타깃을 다시 호출하던 단계만 제거했다.

`proxy_tests`는 128콜백 예열 후 정상 복제와 링 포화를 각각 10,000회 측정한다.
정상 경로는 매번 측정 밖에서 PCM을 소비·검증하고 게시/소비 증가·드롭 0을 확인한다.
포화 경로는 64슬롯을 미리 채우고 게시 증가 0·드롭 10,000을 확인한다. 각 경로의 평균·
p50/p95/p99·최대를 따로 출력한다. 시간에는 가짜 호스트의 PCM 쓰기와 프록시가 포함되고
소비자 처리는 제외된다. CI 스케줄링 변동 때문에 시간 임계값으로 합격을 결정하지 않으며,
정상/포화 기능 카운터와 할당 검사를 합격 조건으로 삼는다. 물리 입출력 지연 측정이 아니다.

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

설정 테스트는 GUID로 구분한 임시 HKCU 하위 키만 사용한다. 기본 꺼짐·장치 저장·공백과
한글 경로 인용·타사 값 보존·잘못된 값·DACL 접근 거부·프로세스 간 단일 실행·비활성
`--startup` 조기 종료를 검사한다. 장치 ID 일치 정책은 실제 장치를 열지 않고 검사한다.
CTest는 설정·권한(`soundfork_relay_settings_tests`, 20초), 단일 실행
(`soundfork_control_instance_tests`, 30초), 비활성 자동 시작
(`soundfork_disabled_startup_tests`, 30초)을 분리한다. UTC 시작 시각·단계별 경과 시간과
자식 PID·대기 결과·종료 코드를 즉시 출력한다. 자식은 10초 내 종료되지 않으면 해당
프로세스만 종료하고 최대 2초간 회수하며, 의도적인 지연 자식의 종료·회수도 검사한다.
`ctest --test-dir build/windows-msvc-x64 -C Release -R 'soundfork_(relay_settings|control_instance|disabled_startup)_tests' -V`
명령으로 상세 진행을 볼 수 있다. 일반 CI·릴리스는 실패 시에도 `*-test-logs` 아티팩트에
`LastTest.log`, `LastTestsFailed.log`를 30일 보관한다. 테스트 실패는 계속 빌드/배포를 막는다.
실제 로그아웃·로그인, 트레이 재실행, 장치 지연 연결, 수동 중지 유지, Windows 시작프로그램
차단과 다른 관리자 계정 제거는 수동 확인이 필요하다. 설치 시험은 격리 runner에서만
자동 시작 등록의 업데이트 유지·제거 정리까지 검사한다. 로컬에서는 설치·등록을 실행하지 않는다.

### 가속 soak와 하드웨어 시험

`control_status_tests`는 한·영 요약 상태의 설정 누락·장치 대기·입력 대기·중지·오류를
검사하며, 프로세스 실행을 정상 송출로 표현하지 않는지 확인한다. `control_ui_tests --test`는
실제 Win32 컨트롤을 숨겨서 만들고 진단 창의 생성·언어 전환·닫기·재생성을 검사한다.
인자 없이 실행하면 읽기 전용 UI 미리보기를 연다. 설치된 제어판의 단일 실행 뮤텍스와
트레이를 사용하지 않으며 설정 저장·온라인 작업·오디오 시작·중지 명령을 차단한다.
사용자의 클립보드를 덮어쓰지 않도록 복사 버튼의 실제 실행은 자동 검사에서 제외한다.
메인 체크박스의 기본 꺼짐·등록 값 재반영·한영 문구·메뉴 중복 제거도 확인한다.
`installer_policy_tests.ps1`은 파일만 읽어 제조사 우선 선택·첫 드라이버 fallback 제거와
설치·등록 스크립트의 필수 `BackendName`을 확인한다. `Test-ReleaseInstaller.ps1`은
격리 CI에서 선택 없는 신규 무인 설치 거부·신규 자동 시작 꺼짐·기존 선택과 자동 시작 유지도 검사한다.
설치 안내는 `SuppressibleMsgBox`를 사용해 무인 실행의 `/SUPPRESSMSGBOXES`를 준수한다.
`release_process_tests.ps1`은 종료 코드 전달과 시간 초과 시 테스트 자식 프로세스까지 종료되는지
확인한다. 설치·업데이트·제거는 단계별 120초, 제어판 자체 검사는 30초 제한이며 CI 단계도 12분으로
제한한다. 실패하더라도 `SoundFork-<version>-installer-logs` 아티팩트에 로그를 30일 보관한다.
관리자 권한이나 설치 변경 없이 실제 초기화 거부 경로를 검사하려면 다음 명령을 사용한다.
원본 `InitializeSetup` 함수를 추출해 합성 드라이버 목록으로 컴파일하며, 설치는 수행하지 않는다.

```powershell
.\tests\installer_rejection_tests.ps1 -CompilerPath 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
```

다른 DPI·복사 성공과 클립보드 점유 실패·실제 로그인 후 동작은 별도 수동 확인 항목이다.

```powershell
.\scripts\Test-SoundForkSoak.ps1 -Hours 4 -Format all -BufferSize 64 -SkipBuild
```

기존 soak는 `e6b4838`에서 추가한 수동 가속 도구였고 `add_test`가 없어 일반 CTest에서
실행되지 않았다. 별도 제외 사유는 기록되지 않았다. 기본 4시간 프로필은 수동으로 유지한다. 대신
`soundfork_soak_ci_tests`가 네 형식 각각 30초 분량을 가속 실행하며 60초 제한으로 일반
Debug/Release CI와 릴리스 CTest에 포함된다. 별도 워크플로 중복 실행은 하지 않는다.
직접 실행은 `soundfork_soak_tests --seconds 30 --format all`, 긴 시험은 위 스크립트를 쓴다.
`--hours`/`--seconds`는 형식별 모의 시간이며 실제 실행 시간이나 오디오 대기가 아니다.
각 블록의 PCM·콜백/게시/소비 수·드롭·언더런·큐를 확인하고 private bytes를 보고한다.
기존 Int32 소비자 읽기·변환 경로도 긴 시험에 유지하며, 다른 형식은 원본 바이트로 검사한다.
가짜 드라이버는 사전 할당된 형식별 바이트 버퍼와 블록 크기 설정을 사용한다. 프록시는
네 형식 × 32/64/256프레임을 검사하고, soak의 `--buffer-size`로 다른 크기도 수동 지정한다.
수동 도구에서 `-SkipBuild`를 생략하면 전체 CTest가 실행된다. OBS 빌드도 유지하려면
`-WithObsPlugin`을 함께 지정한다. 이미 CTest가 실행한 릴레이 드리프트 시험은 재호출하지
않으며, `-SkipBuild`일 때는 별도로 실행한다. 실제 시간 기반 시험은 하드웨어 watcher로 분리한다.

이 명령은 형식별로 48 kHz / 64 samples의 4시간 등가 10,800,000콜백과 ±1,500 ppm 드리프트를
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

OBS 플러그인 수정 후에는 같은 곡을 전용 소스와 `CABLE Input` 출력 캡처로 각각
녹화해 비교한다. 중복 캡처는 끄고 마이크·브라우저 동시 녹화, 게임 종료·재시작도 시험한다.
최소 10분 비교 후 수 시간 동시 soak에서 소리와 영상 싱크, 위 통계의 증가량을 확인한다.
고정 영상/음성 오차와 시간이 지날수록 커지는 드리프트를 구분하고, 기존 OBS 오프셋을
기록한다. 설치된 DLL은 자동 교체하지 않으며 사용자 검증 전 수정 완료를 음질 보장으로 해석하지 않는다.

### 진단 로그

v1.0.1부터 제공하며 기존 설치본에는 업데이트 후 적용된다.
제어판의 **도움말 → 로그 폴더 열기** 또는 `%LOCALAPPDATA%\SoundFork\Logs`에서 확인한다.
`control.log`, `relay.log`, `hub.log`는 각각 현재 파일과 `.1`, `.2` 백업을 유지한다.
파일당 1 MiB, 정상적인 자체 기록 기준 총 9 MiB까지이며 오래된 내용은 회전 시 교체한다.
사용자 로그는 프로그램 제거 시 자동 삭제하지 않는다.
UTC 시각(`Z`), PID, 버전과 구조화된 이벤트를 기록한다. UTC에 9시간을 더하면 한국 시각이다.
오디오 PCM·마이크·장치 이름/ID·공유 창 제목은 기록하지 않는다. 경로에는 Windows 사용자
이름이 포함될 수 있으므로 공유 전 검토한다. OBS 소스의 기존 상세 통계는 OBS 로그를 사용한다.

- 제어판: 시작 출처(로그인/수동), 전달 시작 요청·프로세스 생성·중지 신호, 관찰된 상태 변화,
  자동 시작 설정 저장 결과와 오류. `spawned`와 중지 신호는 실제 청취 성공/종료 완료의 증거가 아니다.
- 릴레이: 연결·연결 대기·세션 재시작·HRESULT·종료 코드, 30초 및 세션 종료 통계.
  언더런/큐 재동기화 관찰은 최대 초당 1회 `relay_audio_gap`으로 요약한다.
  초기 충전 무음(`prebuffer_frames`)과 실제 입력 부족(`underflow_frames`)을 구분한다.
- 허브: 소스 연결·해제, 30초 주기의 ASIO 생산자/허브 출력 드롭. 큐/드롭 수치는
  해당 스트림의 통계이며 세션/스트림 재연결 시 일부 값은 초기화된다. 장치 재연결과 게임
  소스 재시작은 같은 의미가 아니므로 `render_session_end`의 이유도 확인한다.

`soundfork_diagnostics`는 ASIO 드라이버/브리지에 링크하지 않는다. UI/캡처 작업자만 고정
128개 대기열에 `try_lock`으로 기록을 전달하고, 별도 작성 스레드만 파일 생성·회전·쓰기를 수행한다.
잠금 경합/대기열 포화는 로그만 생략하며 `logger_dropped`로 알린다. 기록 실패는 재시도
간격을 두고 무시하며 오디오 오류로 전파하지 않는다. 로그 종료의 drain/join은 프로세스 종료
단계에서만 수행한다. ASIO 콜백에서 이 API를 호출하지 않는다.
충돌·강제 종료·디스크 장애 시 마지막 기록이 유실될 수 있고 로그 부재가 무고장을 증명하지 않는다.
진단 창의 `logger_dropped`, `logger_io_errors`는 제어판 작성기의 상태다.

`diagnostics_tests`는 임시 폴더에서 내용·줄바꿈 처리·회전·재시작·동시 생산자 포화·
경로 실패·파일 잠금 후 복구와 하드링크를 통한 다른 파일 덧쓰기 거부를 검사한다.
설치된 로그·설정은 사용하지 않는다. 실제 제어판의 폴더 열기와 새 빌드로 반복 시작/중지·
장치 재연결 시 로그 연계는 별도 수동 확인 항목이다.

## 1.1.0-dev 원본 PCM 복제와 소비자 변환

### 지원 형식과 재생 경로

48 kHz·출력 채널 0/1·스테레오의 `ASIOSTInt16LSB`, `ASIOSTInt24LSB`,
`ASIOSTInt32LSB`, `ASIOSTFloat32LSB`를 복제한다. Float64, MSB,
Int32LSB16/18/20/24, DSD, 다채널과 다른 샘플레이트는 제외한다.
미지원·좌우 형식 불일치는 재생을 거부하지 않고 복제만 비활성화한다.
제어판 진단에 이유를 표시하며 전체 ASIO 드라이버 호환성을 보장하지 않는다.

`createBuffers`에서 형식·크기를 검증하고 복사 길이를 캐시한다. 실제 버퍼 포인터,
콜백과 `outputReady` 순서·게시 시점은 유지한다. ASIO 콜백은 좌우를 각각 한 번씩
`memcpy`하며 변환·리샘플링·추가 대기 큐·로그를 수행하지 않는다.
샘플레이트 변경 통지(48 kHz 이외)나 reset 요청 시 원자적 상태 변경으로 복제를 중단한다.
같은 스트림에서 다시 활성화하지 않고 안전한 버퍼 재생성 시 형식을 다시 연다.
통지 없이 변경되는 드라이버 형식을 콜백에서 질의해 탐지하지는 않는다.

### v3 전달 계약

- 입력은 `Local\SoundFork.Audio.v3`, 출력은 `Local\SoundFork.CaptureHub.v2.OBS`와 `.Relay`다.
  형식은 독립 wire enum S16LE/S24LE/S32LE/F32LE이며 배치는 `[왼쪽 전체][오른쪽 전체]`다.
- 헤더에는 형식·planar 배치·샘플 바이트 수·채널 수·샘플레이트·프레임 수·payload 길이가 있다.
  허용 값·범위·체크섬·길이 일치와 세대 번호를 검증하고, 복사에는 내부 캐시 길이만 사용한다.
  오른쪽 오프셋은 payload의 절반이다. 임의의 외부 오프셋을 받지 않는다.
- 64슬롯·최대 8192프레임·최대 4바이트·2채널을 유지한다. 슬롯당 최대 64 KiB,
  데이터 약 4 MiB를 시작 전에 할당·접근한다. 용량은 실제 대기 시간과 다르다.
- 허브는 변환 없이 두 원본 출력에 분배한다. 소비자 변환은 링의 소비자 읽기 API에서만 수행한다.
  `read_planar`는 바이트 복사, `read_int32_stereo`와 `read_float_stereo`는 작업자용 변환 API다.
- 미지원 복제 진단은 ready가 꺼진 매핑에 보관한다. 이때 형식 필드는 실제 장치 형식이 아닌
  진단용 placeholder이며, 소비자는 연결하지 않고 제어판도 이를 실제 포맷으로 표시하지 않는다.
- 구 v2와 이중 게시하거나 fallback하지 않는다. 드라이버·허브·릴레이·OBS를 같은 빌드로
  업데이트한다. 제어판은 구 입력 매핑이나 포함된 플러그인과 다른 설치 버전을 안내한다.

### 필요한 곳에서만 변환

릴레이는 소비자 작업자에서 S16×65536, 부호 확장 S24×256, S32 그대로를 사용한다.
F32→Int32는 NaN/Inf를 0으로 만들고 [-1,1]로 제한한 뒤 2³¹을 곱해 최근접
(동률은 0에서 멀어짐) 반올림·Int32 포화를 적용한다. 음수 signed left shift는 사용하지 않는다.
이후 WASAPI·드리프트 보정은 기존 정책을 유지한다.

OBS는 정수 입력에 기존 Int32 보간·출력을 유지하고, F32는 float 소스·보간·출력을 사용한다.
유한 float 값은 보존하고 NaN/Inf만 0으로 만든다. Int32 왕복 변환은 하지 않는다.
기존 타임스탬프·지터 버퍼·고수위 재동기화·드리프트·목표 프레임 정책은 유지한다.

### 자동 시험과 수동 검증

변환기 경계값·Int24 부호·비정렬 입력·float 비유한/포화, 네 형식의 원본 바이트 동일성,
무할당 콜백·소비자 정지 격리·게시 횟수·헤더 변조·세대 변경·허브 분배를 검사한다.
소비자 보간에는 정수 정규화와 Int32로 표현되지 않는 float 보존 시험이 있다.
실제 libobs는 네 형식의 파형·좌우·타임스탬프·지터와 정수/float 입력 중단 복구를 검사한다.

실제 FiiO 48k/64, Discord·OBS 수 시간 동시 사용과 다른 형식의 실제 드라이버 시험은
별도 수동 검증이다. 합성 콜백 시간·가속 드리프트 시험은 물리 지연이나 실시간 soak가 아니다.
정식 다운로드는 1.0.1을 유지하며 개발 빌드가 설치본을 자동 교체하지 않는다.

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
설치·등록 스크립트 모두 `-BackendName`이 필수이며 제조사 기본값은 없다.
설치·COM 등록·제거 자동 시험은 격리된 CI runner에서 임시 ASIO 백엔드로 수행한다.
`Test-ReleaseInstaller.ps1 -AllowRegistryChanges`는 실제 레지스트리를 바꾸며 기존
SoundFork 키를 발견하면 중단한다. 사용 중인 PC에서 무심코 실행하지 않는다.

## 패키징과 릴리스

### 로컬 패키지

Inno Setup 6이 있으면 Setup.exe와 ZIP, `SHA256SUMS.txt`를 `artifacts`에 만든다.
아래 버전은 로컬 패키징 예시이며 이미 존재하는 태그를 재생성하라는 뜻이 아니다.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 1.1.0-alpha.1
.\scripts\Build-ReleasePackage.ps1 -Version 1.1.0-alpha.1 -SkipInstaller
```

인증서가 있으면 모든 배포 EXE·DLL과 Setup을 SHA-256 / RFC 3161로 서명·검증한다.
인증서 비밀키·비밀번호는 저장소에 넣지 않는다.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 1.1.0-alpha.1 `
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
`-SkipTests`로 테스트 소스까지 컴파일하되 자식 프로세스를 실행하는 CTest는 추적 환경에서
중복 수행하지 않는다. 전체 실행 검사는 일반 Debug·Release CI와 릴리스에 유지한다.
빌드 정책 자동 시험은 이 옵션이 CodeQL에만 전달되고 실패 로그 보관이 유지되는지 검사한다.
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
