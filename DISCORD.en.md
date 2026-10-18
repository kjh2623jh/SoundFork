# Discord integration status

[한국어](DISCORD.md) | **English**

## Confirmed public behavior

Discord's current help documentation says the Windows desktop client can share
an individual application or an entire screen with audio. It does not document
an API for third-party applications to submit PCM to a Go Live stream:

- https://support.discord.com/hc/en-us/articles/360040816151-Go-Live-and-Screen-Share
- https://support.discord.com/hc/en-us/articles/9410427556375--Windows-Capturing-Application-Window-for-Screen-Share-and-Go-Live

Microsoft's process-loopback documentation says a process with no Windows audio
render streams produces silence. That explains why a pure ASIO stream cannot be
assumed to appear in application audio capture:

- https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/

Windows 11 build 22621 added `IAudioViewManagerService`, a public API that
associates a WASAPI audio stream with an `HWND`:

- https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioviewmanagerservice

SoundFork uses this API only as documented. It does not inject into Discord or
DJMAX and does not call Discord private APIs.

## Test matrix

Use a remote listener; Discord's local preview is not sufficient evidence.

1. **DJMAX application share, relay off** — tested; ASIO audio was absent.
2. **DJMAX application share, relay on** — tested; Discord did not include the
   relay audio even though Windows accepted the HWND association.
3. **Entire-screen share, relay on** — tested with VB-CABLE; the viewer received
   relay audio. This is the current supported Phase 4 path.
4. **Entire-screen share, relay off** — tested; direct ASIO alone was absent.

Run the relay with automatic endpoint selection (VB-CABLE first, then
VoiceMeeter):

```powershell
.\scripts\Start-SoundForkRelay.ps1
```

Do not run `soundfork_capture_monitor` at the same time because the shared ring
is deliberately SPSC. Do not enable Windows **Listen to this device** on
`CABLE Output` or route the selected virtual endpoint back to FiiO; doing so
would create an audible delayed duplicate. SoundFork does not change the
default render or capture device, and `CABLE Output` is not selected as the
Discord microphone.

## Why application sharing is different

Application-window sharing and entire-screen sharing cross different audio
boundaries. The SoundFork relay creates a Windows audio render stream owned by
`soundfork_discord_relay.exe`, not DJMAX. A capture restricted to the DJMAX
process therefore sees no Windows render stream and receives silence. Entire-
screen/system capture can include the relay stream sent to VB-CABLE.

The public `IAudioViewManagerService` call associates an HWND with an audio
stream. The tested Discord client did not use that association to include the
external relay stream in DJMAX application sharing. It is not an API for
injecting PCM into Discord.

## Decision boundary

Discord ignored the HWND association for application-window audio. There is no
documented Discord insertion API to fall back to. The current verified route is
entire-screen sharing with the relay. Remaining alternatives are:

- share a dedicated SoundFork video/audio relay window; or
- provide a signed Windows virtual audio endpoint based on the documented
  WaveRT/SysVAD architecture.

SoundFork currently detects an independently installed base VB-CABLE first and
uses an installed VoiceMeeter endpoint as a fallback. It does not bundle or
silently install either third-party driver.
