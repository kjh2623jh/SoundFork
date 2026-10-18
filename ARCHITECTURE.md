# SoundFork 아키텍처

**한국어** | [English](ARCHITECTURE.en.md)

## 재생 데이터 경로

```text
64비트 ASIO 호스트(DJMAX, 다른 게임/DAW, soundfork_host)
        |
        | IASIO COM 호출과 콜백
        v
soundfork_driver.dll
        |
        | 제어 호출 위임
        | 하위 드라이버 버퍼 포인터 직접 노출(재생 zero-copy)
        v
설정된 하드웨어 ASIO 드라이버
        |
        v
DAC
```

`createBuffers`는 작은 `ASIOBufferInfo` 설명자 배열만 복사한다. 하위 드라이버가
제공한 버퍼 포인터를 호스트 설명자에 그대로 기록하므로, `bufferSwitch` 중 게임은
물리 드라이버가 소유한 버퍼에 직접 쓴다.

현재 호스트 프로세스마다 준비된 스트림 하나만 허용한다. ASIO 콜백 함수 포인터에는
컨텍스트 값이 없으므로 활성 인스턴스를 원자적 포인터 하나로 보관한다. 두 번째 활성
`createBuffers` 요청은 `ASE_InvalidMode`를 반환한다.

## 실시간 처리 규칙

기본 재생 전달 경로에는 다음만 포함된다.

- 원자적 포인터 읽기 한 번
- null 검사 한 번
- 호스트 콜백 직접 호출 한 번

동적 할당, 파일·레지스트리 접근, 로그, 잠금, 대기, 네트워크 접근, 샘플 변환,
추가 재생 버퍼가 없다. 캡처 링에 여유가 있으면 64프레임 스테레오를 한 번
인터리브 복사한다. 링이 가득 차면 카운터 확인과 드롭 카운터 증가만 수행한다.
자동 테스트는 전역 할당 연산자를 교체하고 캡처 발행을 포함한 콜백 10,000회 동안
할당이 0회인지 검증한다.

## Phase 3 캡처 경로

초기 지원 포맷인 48 kHz 스테레오 `ASIOSTInt32LSB`에서는 완료된 호스트 출력
버퍼를 이름 있는 공유 메모리 SPSC 링에 복사한다.

```text
DJMAX가 FiiO 소유 더블 버퍼에 기록
        |
        +--> FiiO outputReady / DAC (기존 zero-copy 재생)
        |
        +--> L/R 인터리브 -> Local\SoundFork.Audio.v1
                                      |
                                      v
                             별도 캡처 소비자 프로세스
```

매핑에는 최대 8,192 스테레오 프레임 크기의 블록 64개가 있다. 생산자와 소비자는
Windows interlocked 연산으로 단조 증가 시퀀스 카운터를 공개한다. 콜백에서는 어느
쪽도 뮤텍스를 잡지 않는다. 소비자가 느리거나 없어서 링이 가득 차면 생산자가
`dropped_blocks`를 증가시키고 즉시 반환한다. 읽지 않은 데이터를 덮거나 공간이
생길 때까지 기다리지 않는다.

ASIO 호스트가 콜백 안에서 `outputReady()`를 호출하면 프록시는 캡처 복제를 먼저
발행한 뒤 FiiO의 `outputReady()`를 호출한다. 호출하지 않는 호스트에서는 호스트
콜백이 반환된 직후 복제한다. 매핑 생성, 포맷 검색, 소유권 중재는 모두 실시간
콜백 밖인 `createBuffers`에서 수행한다.

`soundfork_capture_monitor`는 진단 소비자이며 `soundfork_discord_relay`는 방송
소비자다. 링은 활성 소비자 하나만 허용한다. `soundfork_control`은 읽기 전용 뷰로
헤더를 관찰하므로 소비자 슬롯을 점유하지 않는다.

실제 FiiO ASIO Driver에서 48 kHz / 64 samples로 검증했다. 직접 경로와 프록시
경로가 모두 168 samples의 출력 지연을 보고했지만, 이는 물리적 왕복 지연 측정값이
아니다.

## 생명주기와 오류

프록시는 `created -> initialized -> prepared -> running` 순서를 강제하며 정리는
역순으로 진행한다. ASIO가 오류 결과를 제공하는 호출에서는 백엔드 오류를 그대로
반환하고, 활성화·설정 오류는 `getErrorMessage`로 확인할 수 있다.

백엔드 선택은 `HKLM\SOFTWARE\ASIO\SoundFork\BackendCLSID`에 저장된다. 등록
스크립트는 선택한 ASIO 드라이버의 기존 레지스트리 항목을 조회한다. SoundFork는
백엔드 드라이버의 COM 등록을 수정하거나 교체하지 않는다.

## Discord와 Windows 오디오 경계

ASIO는 Windows 공유 오디오 엔진을 우회한다. 일반적인 WASAPI 루프백은 Windows
엔드포인트에 렌더링된 스트림을 캡처하므로 FiiO 직접 ASIO 재생이 Discord에 나타난다고
가정할 수 없다. 애플리케이션 창 공유, 전체 화면 공유, 별도 가상 엔드포인트를 각각
시험해야 한다.

## Phase 4 Discord 릴레이

`soundfork_discord_relay`는 Phase 3 SPSC 스트림을 소비하고 48 kHz 스테레오
`Int32LSB`를 선택한 WASAPI 공유 모드 엔드포인트에 렌더링한다. 엔드포인트 포맷이
다르면 Windows가 샘플레이트를 변환한다. 릴레이는 별도 프로세스이므로 ASIO 스케줄링에
영향을 줄 수 없으며, 언더플로가 발생하면 방송 스트림에만 무음이 들어간다.

Windows 11 build 22621 이상에서는 `IAudioViewManagerService`로 오디오 스트림과
DJMAX `HWND`를 연결할 수 있다. 실제 Discord 테스트에서는 이 연결을 애플리케이션
창 공유가 포함하지 않았다. 전체 화면 공유에는 릴레이 오디오가 포함됐으므로 현재
지원 모드인 `--system-share`는 효과가 없었던 HWND 연결을 생략한다. Discord 코드
주입이나 비공개 API는 사용하지 않는다.

WASAPI 스트림에는 물리 또는 가상 렌더 엔드포인트가 필요하다. 들리는 물리 출력으로
복제하면 로컬 에코가 생길 수 있다. 현재는 독립 설치된 VB-CABLE을 우선 사용하고
VoiceMeeter 엔드포인트를 대안으로 지원한다. 모니터링은 꺼야 한다. 영구 가상
엔드포인트를 SoundFork가 직접 제공하려면 서명된 WaveRT/가상 오디오 드라이버가
필요하며, 사용자 모드 API만으로는 만들 수 없다.

## Phase 5 컨트롤 앱

`soundfork_control`은 일반 사용자 권한으로 실행되는 선택적 Win32 앱이다. 등록된
x64 ASIO 드라이버와 활성 WASAPI 렌더 엔드포인트를 열거하고, SoundFork COM 등록과
공유 링 카운터를 보여주며, 전체 화면 릴레이를 시작하거나 중지한다.

하드웨어 백엔드를 바꿀 때만 같은 실행 파일의 작은 관리자 명령을 실행해
`BackendCLSID`를 갱신한다. UI와 릴레이는 관리자 권한을 유지하지 않는다. 컨트롤
앱이 열려 있지 않아도 ASIO 드라이버와 DAC 재생은 정상 동작한다.

방송 엔드포인트 선택은 다음 원칙을 따른다.

1. 저장된 선택이 현재도 유효한 지원 가상 엔드포인트면 유지한다.
2. 그렇지 않으면 기본 VB-CABLE을 선택한다.
3. VB-CABLE이 없으면 VoiceMeeter Desktop, 그다음 다른 VoiceMeeter 엔드포인트를
   선택한다.
4. 물리 장치는 자동 선택하지 않으며 Windows 기본 오디오 장치를 변경하지 않는다.

## 설치 및 릴리스 경계

Inno Setup 설치 프로그램은 등록된 x64 ASIO 백엔드를 선택받은 뒤 관리자 권한으로
`soundfork_control --register-driver --backend-clsid <GUID>`를 실행한다. 이 helper는
실행 파일과 같은 디렉터리의 `soundfork_driver.dll`만 로드할 수 있으며, 선택한
CLSID가 기존 ASIO 등록에 있는지 확인한다. DLL 등록 뒤 백엔드 저장에 실패하면
SoundFork 등록 전체를 롤백한다. 제거는 `--unregister-driver`를 사용하며 다른 ASIO
드라이버나 가상 오디오 장치를 건드리지 않는다.

Release 빌드는 정적 MSVC 런타임을 사용한다. GitHub 태그 파이프라인은 CMake
버전과 태그를 비교하고, 빌드·CTest·PE 의존성 검사·설치/제거 smoke test를 통과한
경우에만 미서명 Setup.exe와 ZIP을 게시한다. 실제 ASIO 하드웨어와 Discord 원격
청취는 호스티드 CI에서 재현하지 않고 수동 릴리스 체크로 유지한다.
