# SoundFork developer documentation

[한국어](DEVELOPMENT.md) | **English** · [User guide](../README.en.md)

This document covers SoundFork's architecture, real-time rules, build and test
workflow, and verified boundaries. See the [README](../README.en.md) for normal
installation and Discord use.

## Components

- `asio-proxy`: x64 COM DLL exposed to ASIO hosts as `SoundFork`
- `audio-bridge`: forwards hardware ASIO lifecycle calls and buffers
- `capture-backend`: nonblocking shared ring, monitor, and WASAPI relay
- `control-app`: backend selection, status, and relay controls
- `tests`: fake ASIO driver and proxy, COM, and shared-ring tests
- `installer`: Inno Setup package definition

With no saved preference, the control UI uses Korean only when the Windows UI
language is Korean and falls back to English for every other language. Setup's
selected language initializes the first value, while later changes through the
**Language/언어** menu persist in `HKCU\Software\SoundFork\Language`.

SoundFork is not DJMAX-specific process hooking. Any x64 game or DAW using the
standard ASIO COM interface can select it. DJMAX RESPECT V and the FiiO K11 R2R
are the current real-hardware validation environment.

## Playback and capture architecture

```text
x64 ASIO host
  -> soundfork_driver.dll
      +-> hardware ASIO driver -> DAC
      +-> shared-memory SPSC ring -> soundfork_discord_relay.exe
                                     -> VB-CABLE -> Discord screen share
```

`createBuffers` exposes the downstream driver's buffer pointers directly to the
host. The game writes into hardware-owned buffers, so playback adds neither an
extra buffer nor sample conversion. ASIO callbacks have no context argument;
therefore one prepared stream is allowed per process and a second
`createBuffers` returns `ASE_InvalidMode`.

For 48 kHz stereo `ASIOSTInt32LSB`, completed blocks are interleaved into
`Local\SoundFork.Audio.v1`. The ring supports 64 blocks of up to 8,192 frames
and one consumer. If that consumer is absent or slow, only capture blocks are
dropped and `dropped_blocks` is incremented. DAC playback never waits.

### Real-time callback rules

The callback performs no dynamic allocation, file/registry/network access,
logging, locks, waits, unnecessary conversion, or waiting for a broadcast
consumer. Automated tests verify zero allocations across 10,000 callbacks,
including capture publication.

## Discord boundary

ASIO bypasses the Windows shared audio engine. Direct FiiO ASIO output is not
expected in ordinary WASAPI loopback. The relay's Windows stream belongs to
`soundfork_discord_relay.exe`, not DJMAX.

Remote-listener tests established:

- DJMAX application-window share: no game audio, even with the relay
- entire-screen share with direct ASIO: no game audio
- entire-screen share with SoundFork relay and VB-CABLE: game audio included

Entire-screen sharing is therefore the supported mode. Associating the stream
with the DJMAX HWND through Windows 11 `IAudioViewManagerService` was also
tested, but Discord did not include that external stream in application-window
sharing. SoundFork uses neither code injection nor private Discord APIs.

Public references:

- <https://support.discord.com/hc/en-us/articles/360040816151-Go-Live-and-Screen-Share>
- <https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/>
- <https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioviewmanagerservice>

## Build

Requirements are Windows 10/11 x64, the Visual Studio 2022 **Desktop
development with C++** workload, and CMake 3.25 or newer.

```powershell
.\scripts\Build-SoundFork.ps1 -Configuration Debug
.\scripts\Build-SoundFork.ps1 -Configuration Release
```

Release binaries use the static MSVC runtime. Outputs are written below
`build/windows-msvc-x64/<Config>`.

The icon source is `assets/soundfork-icon.png`. Regenerate the multi-size
Windows icon with:

```powershell
.\scripts\Build-Icon.ps1
```

Creating Setup.exe locally requires Inno Setup 6.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-alpha.1

# Verify only the ZIP without Inno Setup
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-alpha.1 -SkipInstaller
```

## Testing

CTest runs six cases covering the proxy core, COM server, ASIO enumeration,
shared ring, WASAPI endpoint enumeration, and control application. As of
2026-10-19, all 6/6 pass in both Debug and Release.

Hardware tests used FiiO ASIO Driver 6.16.0.0 and a FiiO K11 R2R at 48 kHz,
64 samples, stereo `ASIOSTInt32LSB`. Both direct and proxy paths reported 168
samples of ASIO output latency. The user verified stereo playback, no
perceptible latency regression, no clicks/pops/dropouts, and successful game
restart. The 168-sample value is driver-reported, not a physical loopback
measurement.

Late and stopped consumer tests confirmed that the FiiO callbacks continue.
The installed COM path delivered 2,244 blocks / 143,616 frames with zero
producer drops during a three-second consumer interval. On 2026-10-19, the
GitHub Release Setup was also installed on a real Windows system and removed
successfully through Windows uninstallation.

### Not currently claimed

- nonzero DJMAX music PCM observed by the shared-memory monitor
- multi-hour capture soak testing
- Discord application-window audio
- simultaneous remote verification of a real microphone and the screen relay
- OBS integration
- physical loopback latency measurement

## Registration and installation

Development registration requires an elevated x64 PowerShell session.

```powershell
.\scripts\Register-SoundFork.ps1 -BackendName 'FiiO ASIO Driver'
.\scripts\Get-SoundForkStatus.ps1 | Format-List
.\scripts\Unregister-SoundFork.ps1
```

Setup asks for a registered x64 ASIO backend and registers only the adjacent
`soundfork_driver.dll`. It rolls back SoundFork registration if backend setup
fails. Uninstall does not modify other ASIO drivers or virtual audio devices.

## CI/CD

Pull requests and pushes to `main` build and test Windows x64 Debug and Release.
The following tag format produces Setup.exe, an executable ZIP, and SHA-256
checksums:

```powershell
git tag -a v0.1.0-alpha.1 -m 'SoundFork v0.1.0-alpha.1'
git push origin v0.1.0-alpha.1
```

Supported suffixes are `alpha.N`, `beta.N`, and `rc.N`; they are published as
GitHub prereleases. A plain `vX.Y.Z` tag creates a normal release. The base tag
version must match `CMakeLists.txt`, and the tagged commit must belong to
`main`. Before tagging a new version, update the direct Setup link in both user
READMEs to that same tag.

Before publishing, the release workflow verifies x64 PE files and runtime
dependencies, checksums, then installation, COM registration, and removal with
a temporary ASIO backend. The installer test refuses to overwrite existing
SoundFork keys. Real hardware and remote Discord listening remain manual
release checks.

## License

SoundFork is GPL-3.0-only. See [third-party notices](../THIRD_PARTY_NOTICES.en.md)
for the ASIO SDK, optional VB-Audio software, and Inno Setup.
