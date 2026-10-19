# 제3자 구성 요소 고지

**한국어** | [English](THIRD_PARTY_NOTICES.en.md)

이 문서는 편의를 위한 한국어 안내다. 정확한 라이선스 문구와 고지 내용은
[영어판](THIRD_PARTY_NOTICES.en.md) 및 각 구성 요소의 원본 라이선스를 함께 확인한다.

## Steinberg ASIO SDK 2.3.4

SoundFork에는 Steinberg ASIO SDK 2.3.4 오픈소스 릴리스의 수정하지 않은 ASIO ABI
헤더 `asiosys.h`, `asio.h`, `iasiodrv.h`가 포함된다. SoundFork는 SDK의 이중
라이선스 중 GPL Version 3 선택지를 사용한다.

- 업스트림 미러: <https://github.com/audiosdk/asio>
- 고정한 업스트림 커밋: `496a0765b8bb9c26f764f22f9a9712a937177db2`
- Steinberg 개발자 포털: <https://www.steinberg.net/developers/>
- SDK 라이선스: `third_party/asiosdk/LICENSE.txt`

SHA-256 체크섬:

| 파일 | SHA-256 |
| --- | --- |
| `common/asiosys.h` | `F53B4C101F84806A9320AEBB24C5B3F7A81DBE7762E558915CA2B2F85DB43BF6` |
| `common/asio.h` | `0159F2F807C49C3B165AAE87DD592DE888AFE9C2E34E1F57E089EC8F1C1A825D` |
| `common/iasiodrv.h` | `C22183608757A6CB483B412829A27DDE37978289D4932E3BA930EE6C20C35290` |
| `LICENSE.txt` | `E664C38C9A99BE3AC32D7E49D56B6E5E0F807943F4229020D2303AD60BD31306` |

ASIO는 Steinberg Media Technologies GmbH의 상표다. SoundFork는 ASIO 로고를
사용하지 않으며 제품 이름에도 ASIO를 포함하지 않는다.

## 선택적 OBS 오디오 소스

선택형 플러그인은 OBS Studio 32.2.2 공식 헤더를 사용합니다. 해당 헤더는
GPL-2.0-or-later와 각 파일의 고지를 따릅니다. 소스 및 SDK 런타임은 Git에서 제외된
빌드 폴더에만 다운로드합니다. 배포물에는 SoundFork의 GPL-3.0-only 플러그인과
캡처 허브만 포함하며 OBS 런타임 DLL은 포함하지 않습니다.

- 원본: <https://github.com/obsproject/obs-studio/tree/32.2.2>
- 라이선스: <https://github.com/obsproject/obs-studio/blob/32.2.2/COPYING>
- 소스 SHA-256: `ec81fb66b03e75ddb3076b576f62679c39262e0e9960cef3e17a40dc5d68e6b4`
- Windows x64 SHA-256: `4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1`

## 선택적 VB-Audio 소프트웨어

SoundFork는 별도로 설치된 VB-CABLE 또는 VoiceMeeter 렌더 엔드포인트를 감지해
Discord 전체 화면 공유에 사용할 수 있다. 해당 제품과 드라이버는 이 저장소나
SoundFork 설치 파일에 포함되지 않으며 VB-Audio Software가 별도로 배포하고
라이선스를 제공한다.

- VB-CABLE: <https://vb-audio.com/Cable/>
- VB-Audio 라이선스: <https://vb-audio.com/Services/licensing.htm>

## Inno Setup 빌드 도구

Windows `Setup.exe`는 Inno Setup 6으로 생성한다. Inno Setup 자체는 SoundFork
소스나 배포 ZIP에 포함하지 않으며, 최종 설치 프로그램만 GitHub Release에
게시한다. Inno Setup은 비상업 사용자에게 라이선스 구매를 요청하지 않지만,
공식 안내가 정의한 상업 사용자는 로컬 또는 CI에서 컴파일러를 사용할 때 상업용
라이선스 구매를 요청받는다. SoundFork를 상업적으로 빌드·배포하려는 사용자는
그 시점의 공식 조건을 확인해야 한다.

- Inno Setup 다운로드: <https://jrsoftware.org/isdl.php>
- 상업용 라이선스 안내: <https://jrsoftware.org/isorder.php>
