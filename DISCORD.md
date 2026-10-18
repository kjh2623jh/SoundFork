# Discord 연동 상태

**한국어** | [English](DISCORD.en.md)

## 확인된 공개 동작

Discord 공식 도움말은 Windows 데스크톱 클라이언트가 개별 애플리케이션 창이나
전체 화면을 오디오와 함께 공유할 수 있다고 설명한다. 제3자 프로그램이 PCM을
Go Live 스트림에 직접 제출하는 API는 문서화되어 있지 않다.

- <https://support.discord.com/hc/en-us/articles/360040816151-Go-Live-and-Screen-Share>
- <https://support.discord.com/hc/en-us/articles/9410427556375--Windows-Capturing-Application-Window-for-Screen-Share-and-Go-Live>

Microsoft의 프로세스 루프백 문서는 캡처 대상 프로세스에 Windows 오디오 렌더
스트림이 없으면 캡처 프로세스가 무음을 받는다고 설명한다. 따라서 순수 ASIO
스트림이 애플리케이션 오디오 캡처에 나타난다고 가정할 수 없다.

- <https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/>

Windows 11 build 22621에는 WASAPI 스트림과 `HWND`를 연결하는 공개
`IAudioViewManagerService`가 추가됐다.

- <https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioviewmanagerservice>

SoundFork는 문서화된 범위에서만 이 API를 사용한다. Discord나 DJMAX에 코드를
주입하지 않고 Discord 비공개 API도 호출하지 않는다.

## 실제 테스트 결과

Discord 로컬 미리보기만으로 판단하지 않고 원격 청취자로 확인했다.

1. **DJMAX 애플리케이션 공유, 릴레이 꺼짐**: ASIO 오디오 없음
2. **DJMAX 애플리케이션 공유, 릴레이 켜짐**: Windows가 HWND 연결을 수락했지만
   Discord가 릴레이 오디오를 포함하지 않음
3. **전체 화면 공유, 릴레이 켜짐**: VB-CABLE을 통해 원격 청취자에게 오디오 전달
4. **전체 화면 공유, 릴레이 꺼짐**: 직접 ASIO 오디오 없음

현재 지원 경로:

```powershell
.\scripts\Start-SoundForkRelay.ps1
```

스크립트는 VB-CABLE을 먼저 찾고 없으면 VoiceMeeter 엔드포인트를 사용한다.
공유 메모리 링은 SPSC이므로 `soundfork_capture_monitor`와 릴레이를 동시에 실행하지
않는다. `CABLE Output`에서 Windows **이 장치로 듣기**를 켜거나 가상 출력을 다시
FiiO로 라우팅하면 지연된 복제음이 들릴 수 있으므로 사용하지 않는다. SoundFork는
Windows 기본 재생·녹음 장치나 Discord 마이크 선택을 바꾸지 않는다.

## 애플리케이션 공유가 다른 이유

```text
DJMAX.exe
  +-> FiiO ASIO                     Windows 렌더 스트림 없음

soundfork_discord_relay.exe
  +-> WASAPI -> CABLE Input         별도 프로세스 소유 스트림
```

SoundFork 릴레이가 만드는 Windows 오디오 렌더 스트림의 소유자는 DJMAX가 아니라
`soundfork_discord_relay.exe`다. DJMAX 프로세스로 제한된 캡처에서는 DJMAX에
Windows 렌더 스트림이 없으므로 무음이 된다. 별도 릴레이 프로세스의 스트림도
DJMAX 프로세스 트리에 속하지 않는다.

반면 전체 화면 공유는 실제 테스트에서 VB-CABLE로 보낸 릴레이 스트림을 포함했다.
Discord의 정확한 내부 캡처 구현은 공개되지 않았으므로 이 설명은 Microsoft의 공개
프로세스 캡처 규칙과 실측 결과를 결합한 결론이다.

`IAudioViewManagerService`는 HWND와 오디오 스트림을 연결하지만 Discord에 PCM을
삽입하는 API는 아니다. 시험한 Discord 클라이언트는 이 연결을 이용해 외부 릴레이
스트림을 DJMAX 애플리케이션 공유에 포함하지 않았다.

## 지원 경계와 대안

현재 안정적으로 검증된 방식은 **전체 화면 공유 + SoundFork 릴레이 + VB-CABLE**이다.
애플리케이션 창 공유를 지원하려면 다음 중 하나가 필요하지만 아직 구현하지 않았다.

- SoundFork 전용 영상·오디오 릴레이 창 자체를 공유
- Discord가 지원하는 공개적인 외부 오디오 삽입 API가 향후 제공됨
- 프로세스 후킹이나 코드 주입 사용 — 기본 설계 원칙상 채택하지 않음

SoundFork는 독립 설치된 기본 VB-CABLE을 자동 감지하고 VoiceMeeter를 대안으로
지원한다. 두 제3자 드라이버를 저장소에 포함하거나 자동·무인 설치하지 않는다.
