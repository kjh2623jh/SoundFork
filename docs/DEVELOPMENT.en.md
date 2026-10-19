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
                                     -> virtual audio render endpoint
                                         +-> Discord entire-screen share
                                         +-> OBS Audio Output Capture
```

`createBuffers` exposes the downstream driver's buffer pointers directly to the
host. The game writes into hardware-owned buffers, so playback adds neither an
extra buffer nor sample conversion. ASIO callbacks have no context argument;
therefore one prepared stream is allowed per process and a second
`createBuffers` returns `ASE_InvalidMode`.

For 48 kHz stereo `ASIOSTInt32LSB`, completed blocks are interleaved into
`Local\SoundFork.Audio.v2`. The ring supports 64 blocks of up to 8,192 frames
and one consumer. If that consumer is absent or slow, only capture blocks are
dropped and `dropped_blocks` is incremented. DAC playback never waits.

Shared-memory v2 validates the header and mapping sizes, all offsets, and a
format checksum. Both processes cache the validated block size internally, so
mutating the shared header cannot enlarge a callback copy. A generation counter
and interlocked sequences reject data across reconfiguration, and every page of
the roughly 4 MiB data area is committed and touched before streaming. This is
defensive memory-safety validation, not a security boundary against a malicious
process running as the same user.

### Real-time callback rules

The callback performs no dynamic allocation, file/registry/network access,
logging, locks, waits, unnecessary conversion, or waiting for a broadcast
consumer. Automated tests verify zero allocations across 10,000 callbacks,
including capture publication.

### Meaning of latency

The game writes directly to FiiO-owned double buffers and returns them to FiiO
within the same callback. No playback queue, worker thread, or one-buffer wait
is inserted, so the theoretical lower bound of SoundFork-added **buffering
latency is 0 ms**. This does not mean actual input-to-output latency is 0 ms.
The ASIO driver, USB, DAC, and game still contribute latency, and no physical
loopback measurement has been performed. Both direct and proxy paths reported
168 samples through the driver, and the user reported no perceptible difference.

CPU time for the capture copy is distinct from an added buffer period. In a
local Release accelerated run on 2026-10-19, sampling every 4,096th callback
across 10,800,000 callbacks (four hours of synthetic ASIO time) measured an
average of 0.121 us and a maximum of 7.7 us. These are wall-clock results for
this machine and fake backend, not measured FiiO latency.

### Broadcast relay stability

FiiO ASIO and a virtual WASAPI device use independent clocks. Outside the game
path, the relay uses adaptive linear resampling around a four-block target and
limits correction to ±2,000 ppm. At 48 kHz/64 samples, four blocks intentionally
add about 5.33 ms to the broadcast path only. A severely stale relay queue is
resynchronized to the newest four blocks without touching DAC playback.

On `AUDCLNT_E_DEVICE_INVALIDATED` and related endpoint/service failures, the
relay destroys the WASAPI session, waits for the endpoint, opens it again, and
reapplies window association. It never stops the ASIO proxy or FiiO playback.
At exit it reports queue minima/maxima, silent frames, peak correction, resyncs,
discarded blocks, endpoint reconnects, and producer drops.

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

In OBS, select the relay's virtual render endpoint as an **Audio Output Capture**
source or under **Settings → Audio → Desktop Audio**. With VB-CABLE, that endpoint
is `CABLE Input`. `CABLE Output` is a recording endpoint and therefore does not
appear in those device lists.

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
.\scripts\Test-SoundForkStaticAnalysis.ps1
```

Release binaries use the static MSVC runtime. Outputs are written below
`build/windows-msvc-x64/<Config>`. MSVC CFG, NX, and ASLR are enabled, and the
release check verifies the PE flags and Guard CF table. Static analysis runs
with `/analyze /WX`, so warnings fail the build.

The icon source is `assets/soundfork-icon.png`. Regenerate the multi-size
Windows icon with:

```powershell
.\scripts\Build-Icon.ps1
```

Creating Setup.exe locally requires Inno Setup 6.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-alpha.2

# Verify only the ZIP without Inno Setup
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-alpha.2 -SkipInstaller
```

With a code-signing certificate, the package script can SHA-256/RFC 3161 sign
and immediately verify every shipped EXE/DLL and Setup. Private keys are never
stored in the repository.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-alpha.2 `
  -CertificateThumbprint '<SHA1 thumbprint>' `
  -TimestampUrl 'https://timestamp-provider.example'
```

No code-signing certificate is installed in the current local environment, so
successful signing is not claimed.

## Testing

CTest runs seven cases covering the proxy core, COM server, ASIO enumeration,
shared ring, relay clock, WASAPI endpoint enumeration, and control application.
As of 2026-10-19, all 7/7 pass in Debug and Release. The proxy, shared-ring,
and relay tests also pass under MSVC AddressSanitizer, and the complete
`/analyze /WX` build is clean.

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
successfully through Windows uninstallation. OBS also captured audio in a
real-use test when Desktop Audio was set to the relay's `CABLE Input` endpoint.

This command accelerates 10,800,000 callbacks representing four hours at
48 kHz/64 samples and also simulates ±1,500 ppm relay drift:

```powershell
.\scripts\Test-SoundForkSoak.ps1 -Hours 4
```

The local accelerated run on 2026-10-19 recorded zero producer drops, zero
consumer underflows, a maximum one-block queue, and identical start/end private
bytes. It was not four hours of real-time hardware playback. For a real FiiO,
Discord, and OBS run, start the relay and use the watcher below; listening and
remote reception still require a person to verify them.

```powershell
.\scripts\Watch-SoundForkHardwareSoak.ps1 -Hours 4
```

### Not currently claimed

- nonzero DJMAX music PCM observed by the shared-memory monitor
- a four-hour real-time simultaneous FiiO, Discord, and OBS soak result
- physical removal/reconnection of the virtual WASAPI device or driver
- Discord application-window audio
- simultaneous remote verification of a real microphone and the screen relay
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

Pull requests and pushes to `main` build and test Windows x64 Debug and Release,
and the Release job runs MSVC analysis with warnings treated as errors.
The following tag format produces Setup.exe, an executable ZIP, and SHA-256
checksums:

```powershell
git tag -a v0.1.0-alpha.2 -m 'SoundFork v0.1.0-alpha.2'
git push origin v0.1.0-alpha.2
```

Supported suffixes are `alpha.N`, `beta.N`, and `rc.N`; they are published as
GitHub prereleases. A plain `vX.Y.Z` tag creates a normal release. The base tag
version must match `CMakeLists.txt`, and the tagged commit must belong to
`main`. Before tagging a new version, update the direct Setup link in both user
READMEs to that same tag.

Before publishing, the release workflow verifies x64 PE files and runtime
dependencies, CFG/NX/ASLR, checksums, then installation, COM registration, and
removal with a temporary ASIO backend. The installer test refuses to overwrite
existing SoundFork keys. Real hardware and remote Discord listening remain
manual release checks. When `WINDOWS_SIGNING_PFX_BASE64`,
`WINDOWS_SIGNING_PFX_PASSWORD`, and `WINDOWS_TIMESTAMP_URL` are all configured,
the workflow signs before publishing; otherwise it emits an explicit unsigned
release warning.

## License

SoundFork is GPL-3.0-only. See [third-party notices](../THIRD_PARTY_NOTICES.en.md)
for the ASIO SDK, optional VB-Audio software, and Inno Setup.
