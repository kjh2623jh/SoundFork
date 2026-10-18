# 테스트

**한국어** | [English](TESTING.en.md)

## Phase 1 자동 테스트

`soundfork_tests`는 48 kHz `ASIOSTInt32LSB` 출력 2개와 고정 64-sample 버퍼를
제공하는 결정적 인프로세스 ASIO 테스트 드라이버를 사용한다. 다음을 검증한다.

- COM 방식 `QueryInterface`, 참조 카운트, 인스턴스 정리
- 기능 조회와 생명주기 전달
- 호스트와 하위 출력 버퍼의 포인터 동일성
- 콜백 10,000회 동안 좌우 샘플 무결성
- 레거시 콜백과 time-info 콜백 전달
- `outputReady` 전달
- 콜백 경로의 동적 할당 0회
- 두 번째 활성 스트림 거부

`soundfork_com_tests`는 등록 없이 빌드된 DLL을 불러 COM export 전체를 검사하고,
SoundFork CLSID로 클래스 팩토리와 `IASIO` 객체를 생성하며, `DllCanUnloadNow`
생명주기를 확인한다. CTest는 읽기 전용인 `soundfork_host --list`도 실행한다.

`soundfork_capture_tests`는 공유 메모리 포맷 협상, 정확한 스테레오 인터리브,
생산자·소비자 독점 소유권, 제한된 용량, 링이 가득 찼을 때의 드롭을 검증한다.
프록시 테스트는 가짜 ASIO 호스트가 쓴 캡처 샘플을 읽고 캡처 발행 콜백 전체에서
할당 추적을 유지한다.

CTest의 `soundfork_discord_relay --list-endpoints`는 활성 WASAPI 렌더 엔드포인트를
열거만 하고 오디오 설정을 바꾸지 않는다. `soundfork_control --self-test`는 UI나
설정 변경 없이 레지스트리 조회, ASIO와 WASAPI 열거, 읽기 전용 공유 링 관찰자를
검사한다. 엔드포인트 종류 분류도 함께 검사한다.

Phase 2에서는 `soundfork_host --proxy-backend <이름>`이 설치된 하드웨어 드라이버를
인프로세스 SoundFork 코어로 감싼다. SoundFork 등록이나 관리자 권한 없이 프록시
데이터 경로를 검증할 수 있다.

## 수동 등록 테스트

사전 승인과 관리자 권한의 64비트 PowerShell이 필요하다.

1. 기존 `HKLM\SOFTWARE\ASIO` 항목을 기록한다.
2. Release DLL을 대상으로 `Register-SoundFork.ps1`을 실행한다.
3. `soundfork_host --list`에 `SoundFork`가 나타나는지 확인한다.
4. 조회 전용 `--driver SoundFork` 검사를 실행한다.
5. `Unregister-SoundFork.ps1`을 실행한다.
6. SoundFork COM·ASIO 키만 사라지고 FiiO 키가 그대로인지 확인한다.

## Phase 2 하드웨어 결과

2026-10-18에 FiiO ASIO Driver 6.16.0.0과 FiiO K11 R2R로 직접 경로 및 인프로세스
SoundFork 프록시 경로를 48 kHz / 64 samples, `ASIOSTInt32LSB` 출력 2채널에서
실행했다. 5초 무음 프록시 실행은 콜백 3,751회를 처리했고, 이후 2초 비교에서는
프록시 1,500회와 직접 경로 1,501회를 처리했다. 두 경로 모두 ASIO reset 요청 없이
시작, 스트리밍, 정지, 버퍼 해제를 완료했다. 30초 soak는 예상값 22,500회에 가까운
22,501회를 처리했고 reset/resync 요청이나 프로세스 오류가 없었다.

활성 설정에서 FiiO 드라이버는 출력 지연 168 samples(3.5 ms)를 보고했다. 버퍼 한
개의 이론적 길이는 `64 / 48,000 = 1.333 ms`다. 전자는 드라이버 보고값, 후자는
계산값이며 물리적 루프백 측정값이 아니다. SoundFork는 FiiO 버퍼를 직접 노출하므로
보고되는 ASIO 지연을 추가하지 않는다.

테스트 호스트의 `sine-left`와 `sine-right` 기본 레벨은 -40 dBFS다. 사용자가
DJMAX의 좌우 스테레오 재생, 직접 FiiO ASIO 대비 체감 지연 차이 없음, 클릭·팝·드롭아웃
없음, 게임 재시작 성공을 확인했다.

## Phase 3 하드웨어 결과

FiiO 백엔드 프록시와 `soundfork_capture_monitor`를 별도 프로세스로 48 kHz /
64 samples에서 실행했다. 4초 소비 구간에 모니터는 2,993블록 / 191,552프레임을
생산자 드롭 없이 읽었고 ASIO 호스트는 5초 동안 콜백 3,751회를 완료했다.

지연 소비자 테스트에서는 재생 시작 2초 뒤 소비자를 연결했다. 소비자가 없는 동안
캡처 블록 1,471개가 드롭됐고, 연결 뒤 복구되어 1,495블록 / 95,680프레임을
소비했다. FiiO 경로는 여전히 콜백 3,751회를 완료하고 정상 정지했다. 방송 소비자가
없거나 늦어도 게임 재생이 분리된다는 것을 확인한다.

등록된 COM 경로(`soundfork_host --driver SoundFork`)도 외부 모니터와 시험했다.
3초 소비 구간에 2,244블록 / 143,616프레임, 드롭 0개를 기록했고 등록 드라이버는
4초 동안 콜백 3,001회를 완료했다.

콜백 오버헤드와 물리적 왕복 지연은 이론적 버퍼 길이 1.333 ms와 구분해 보고해야
한다.

## Phase 4 Discord 결과

현재 Windows 빌드에서 `IAudioViewManagerService`가 SoundFork 릴레이 스트림과
표시 중인 테스트 창을 정상 연결했다. 초기 VoiceMeeterDesktop 시험에서 릴레이는
96,960프레임을 언더플로와 캡처 드롭 없이 렌더링했고 FiiO ASIO 경로도 정상 동작했다.

원격 Discord 청취 시험 결과는 다음과 같다.

- DJMAX 애플리케이션 창 공유: 연결된 릴레이 오디오가 포함되지 않음
- 전체 모니터 공유 + 릴레이: 오디오 포함
- 전체 모니터 공유 + 직접 ASIO만 사용: 오디오 없음
- 전체 모니터 공유 + VB-CABLE 릴레이: 오디오 포함

따라서 `--system-share`가 현재 검증된 모드다. 대상 창이나 Windows 11 HWND 연결을
요구하지 않는다.

## 아직 구현·검증했다고 주장하지 않는 항목

- 공유 메모리 모니터에서 0이 아닌 DJMAX 곡 PCM 캡처 확인
- 수 시간 규모의 캡처 soak 테스트
- Discord 애플리케이션 창 공유 오디오 전달
- 실제 마이크와 전체 화면 릴레이의 동시 원격 확인
- OBS 연동
- 물리 루프백 장비를 사용한 실제 지연 측정

## Phase 5 컨트롤 앱 체크리스트

1. 관리자 권한 요청 없이 등록된 SoundFork DLL과 선택된 하드웨어 ASIO 백엔드를
   표시한다.
2. 백엔드 목록에서 SoundFork 자신을 제외하고 등록된 x64 ASIO 드라이버를 표시한다.
   변경 적용 시에만 UAC를 요청하며 호스트 재시작 뒤 적용된다.
3. 활성 WASAPI 렌더 엔드포인트를 표시하고 선택을 사용자 레지스트리에 저장한다.
   기본 VB-CABLE, VoiceMeeter Desktop 순으로 우선하며 물리 스피커를 자동 선택하지
   않는다.
4. 릴레이 시작 시 숨겨진 영구 릴레이 프로세스 하나를 만들고, Stop은 강제 종료가
   아니라 정상 종료 신호를 보낸다.
5. ASIO 스트림 실행 중 포맷, 생산자·소비자 PID, 블록 카운터, 캡처 드롭이 소비자
   슬롯을 점유하지 않고 갱신된다.
6. 컨트롤 창을 닫아도 DAC 재생과 활성 릴레이가 중단되지 않는다.
7. 지원되는 가상 엔드포인트가 없으면 **Broadcast: Not ready**를 표시하고
   **Get VB-CABLE**이 <https://vb-audio.com/Cable/>을 연다.
8. VB-CABLE 설치 뒤 장치를 새로 고치면 **Recommended** 표시와
   **Broadcast: Ready (VB-CABLE)** 상태를 확인한다.

## 최신 자동 테스트 결과

2026-10-19 Debug와 Release 빌드에서 각각 CTest 6개가 모두 통과했다.

- 프록시 코어
- COM 서버
- ASIO 호스트 열거
- 공유 메모리 캡처 링
- Discord 릴레이 엔드포인트 열거
- 컨트롤 앱 자체 검사

## CI 및 배포 패키지 테스트

PR과 `main` push에서는 Windows x64 Debug/Release 빌드와 위 CTest 전체를 실행한다.
태그 workflow는 추가로 다음을 확인한다.

1. 태그의 기본 SemVer와 CMake 프로젝트 버전 일치
2. 태그 커밋이 `main`에 포함됨
3. Release PE 파일이 x64이며 동적 MSVC 런타임에 의존하지 않음
4. Setup.exe와 ZIP의 SHA-256 재계산 결과 일치
5. 임시 x64 ASIO 레지스트리 백엔드를 이용한 자동 설치
6. SoundFork COM/ASIO 등록과 선택한 `BackendCLSID` 확인
7. 설치된 컨트롤 앱 자체 검사
8. 자동 제거 뒤 SoundFork 키만 제거되고 가짜 백엔드는 보존됨

`Test-ReleaseInstaller.ps1`은 HKLM을 임시 변경하므로 격리된 관리자 테스트 머신에서
명시적으로 `-AllowRegistryChanges`를 전달해야만 실행된다. 실제 개발 PC에서는
기본적으로 실행하지 않는다. 기존 SoundFork 또는 테스트용 레지스트리 키를 발견하면
덮어쓰지 않고 중단한다.
