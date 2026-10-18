# SoundFork

[한국어](README.md) | **English**

> This project was developed with AI assistance. Architecture decisions, code
> review, and real-hardware and Discord validation were performed by the project
> maintainer.

Documentation: [Architecture](ARCHITECTURE.en.md) ·
[Testing](TESTING.en.md) · [Discord](DISCORD.en.md) ·
[Third-party notices](THIRD_PARTY_NOTICES.en.md)

SoundFork is an experimental Windows x64 user-mode ASIO proxy. An ASIO host
loads `soundfork_driver.dll`; SoundFork opens a configured hardware ASIO driver
and exposes the hardware driver's double-buffer addresses directly to the host.
The playback path adds no buffer and performs no playback sample conversion.
For 48 kHz stereo `ASIOSTInt32LSB`, completed output buffers are also copied
into a nonblocking shared-memory SPSC ring for an external capture consumer.

The current target is DJMAX RESPECT V at 48 kHz / 64 samples with the FiiO ASIO
Driver. The proxy and direct paths have both completed real-device tests at
that configuration, and DJMAX playback has been user-validated for stereo,
stability, restart behavior, and no perceptible latency regression. Discord
entire-screen audio sharing has also been user-validated through the current
experimental relay. Discord application-window sharing did not include the
relay session. OBS capture has not yet been validated.

## Build

Requirements:

- Windows 10/11 x64
- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.25 or newer (the Visual Studio bundled copy is supported)

From PowerShell:

```powershell
.\scripts\Build-SoundFork.ps1 -Configuration Debug
.\scripts\Build-SoundFork.ps1 -Configuration Release
```

The main outputs are:

- `build/windows-msvc-x64/<Config>/soundfork_driver.dll`
- `build/windows-msvc-x64/<Config>/soundfork_control.exe`
- `build/windows-msvc-x64/<Config>/soundfork_host.exe`
- `build/windows-msvc-x64/<Config>/soundfork_tests.exe`

## Install, status, run, and remove

### GitHub Release installer

Most users should download `SoundFork-Setup-vX.Y.Z.exe` from GitHub Releases.
Setup lists the registered x64 hardware ASIO drivers and prefers FiiO ASIO
Driver when it is available. After installation, open **SoundFork Control**
from the Start menu to inspect status and select the broadcast endpoint.

Initial v0.x packages are unsigned alpha builds and may trigger Windows
SmartScreen. Verify downloads against `SHA256SUMS.txt` in the same Release.
SoundFork Setup does not bundle, modify, or remove VB-CABLE, VoiceMeeter, or a
hardware ASIO driver.

The advanced `SoundFork-vX.Y.Z-windows-x64.zip` contains binaries, manual
install/removal scripts, and Korean/English documentation. GitHub's automatic
“Source code” archives are not runnable Windows distributions.

### Install from a source checkout

SoundFork has two different runtime pieces:

- `soundfork_driver.dll` is not a standalone process. DJMAX loads it after
  `SoundFork` is selected as the ASIO driver, so it normally has no separate
  Task Manager entry.
- `soundfork_discord_relay.exe` is a standalone process. It appears in Task
  Manager only while Discord entire-screen audio relay is running.
- `soundfork_control.exe` is the optional Phase 5 control application. It can
  select any registered 64-bit hardware ASIO backend, show live capture status,
  and start or gracefully stop the Discord relay.

The earlier registration command points Windows directly at the DLL under the
build directory. That is suitable for development, but moving or deleting the
build directory breaks registration. For a stable copy, close DJMAX and the
relay, open **64-bit PowerShell as Administrator**, and run:

```powershell
.\scripts\Install-SoundFork.ps1
```

This copies the Release binaries to `C:\Program Files\SoundFork`, registers
only SoundFork, and configures `FiiO ASIO Driver` as its backend. It does not
remove or modify FiiO or another ASIO driver.

Status does not require Administrator:

```powershell
.\scripts\Get-SoundForkStatus.ps1 | Format-List
```

The graphical control application is available after a Release build or stable
installation:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_control.exe
```

Changing the hardware ASIO backend requests UAC only when **Apply** is clicked.
The control application itself runs as a normal user. Backend changes take
effect the next time an ASIO host opens SoundFork. Select a virtual render
endpoint and press **Start relay** for the validated Discord entire-screen
path. The endpoint selector automatically prefers an installed base VB-CABLE,
then a VoiceMeeter desktop endpoint. It never automatically selects a physical
speaker or changes the Windows default device. When neither supported virtual
endpoint is present, **Get VB-CABLE** opens the vendor's official download
page. Closing the control window does not stop an already running relay.

For the currently validated Discord entire-screen path, start and stop the
relay with:

```powershell
.\scripts\Start-SoundForkRelay.ps1
.\scripts\Stop-SoundForkRelay.ps1
```

Then choose `SoundFork` in DJMAX and share the entire monitor in Discord. The
script uses VB-CABLE when available, otherwise it falls back to an installed
VoiceMeeter desktop endpoint. VoiceMeeter's application can be closed while its
virtual audio driver and endpoints remain installed, which is why that name can
still appear. The virtual endpoint carries only the asynchronous broadcast
copy; it is not part of the hardware ASIO playback path.

VB-CABLE is a separate donationware product and is not included in SoundFork.
Download and install it from <https://vb-audio.com/Cable/>, run its setup as
Administrator, and reboot as directed by the vendor. SoundFork does not change
Discord's microphone input: keep the real microphone selected there.

To uninstall, close DJMAX and the relay, open **64-bit PowerShell as
Administrator**, and run:

```powershell
.\scripts\Remove-SoundFork.ps1
```

Removal unregisters SoundFork and deletes only its known files under
`C:\Program Files\SoundFork`. It does not uninstall the FiiO driver or
VoiceMeeter. The older `Register-SoundFork.ps1` and
`Unregister-SoundFork.ps1` remain available for build-tree development.

When an ASIO host invokes SoundFork's ASIO control-panel method, SoundFork
currently forwards it to the selected hardware backend control panel. The
separate SoundFork control application manages relay start/stop, registration
state, capture counters, and backend selection; it is not required for
playback.

## Safe test sequence

Automated tests do not change the registry or open the FiiO device:

```powershell
& .\build\windows-msvc-x64\Debug\soundfork_tests.exe
& .\build\windows-msvc-x64\Debug\soundfork_host.exe --list
```

Registration requires an explicitly elevated 64-bit PowerShell. Review the
script before running it:

```powershell
.\scripts\Register-SoundFork.ps1 `
  -DllPath .\build\windows-msvc-x64\Release\soundfork_driver.dll `
  -BackendName 'FiiO ASIO Driver'
```

To remove SoundFork without touching FiiO's registration:

```powershell
.\scripts\Unregister-SoundFork.ps1 `
  -DllPath .\build\windows-msvc-x64\Release\soundfork_driver.dll
```

After registration, a query-only probe is:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_host.exe --driver SoundFork
```

Before registration, the same proxy core can wrap an installed backend directly:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_host.exe `
  --proxy-backend 'FiiO ASIO Driver'
```

An actual stream test opens the configured hardware driver. Before registration,
use `--proxy-backend 'FiiO ASIO Driver'`; after registration, use
`--driver SoundFork`:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_host.exe `
  --proxy-backend 'FiiO ASIO Driver' --sample-rate 48000 --buffer-size 64 `
  --channels 0,1 --duration 10 --signal silence
```

For audible channel verification, lower the physical output volume first. The
test tone defaults to a conservative -40 dBFS and can be sent separately to
each side:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_host.exe `
  --proxy-backend 'FiiO ASIO Driver' --sample-rate 48000 --buffer-size 64 `
  --channels 0,1 --duration 2 --signal sine-left --gain-db -40

& .\build\windows-msvc-x64\Release\soundfork_host.exe `
  --proxy-backend 'FiiO ASIO Driver' --sample-rate 48000 --buffer-size 64 `
  --channels 0,1 --duration 2 --signal sine-right --gain-db -40
```

Do not use a louder `--gain-db` value until the signal path and volume setting
are known to be safe.

## Shared-memory capture monitor

Phase 3 includes a separate diagnostic consumer. Start it before or while a
SoundFork stream is active:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_capture_monitor.exe `
  --wait 30 --duration 10
```

It reports the negotiated format, consumed blocks and frames, peak PCM
magnitude, and producer-side drops. It does not play or save audio. Only one
consumer may attach at a time. A stopped or late consumer can fill the 64-block
ring, but the ASIO callback never waits; capture blocks are dropped and playback
continues.

## Discord entire-screen relay

Discord does not publish an API for inserting PCM directly into a Go Live audio
track. On Windows 11 build 22621 or later, SoundFork can experimentally render its
captured PCM through a WASAPI session and use `IAudioViewManagerService` to
associate that session with the DJMAX window. Whether Discord honors that public
Windows association must be verified with a remote viewer.

List available render endpoints:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_discord_relay.exe `
  --list-endpoints
```

Only one shared-memory consumer can run, so stop `soundfork_capture_monitor`
before starting the relay. With DJMAX running through SoundFork, start:

```powershell
& .\build\windows-msvc-x64\Release\soundfork_discord_relay.exe `
  --endpoint 'VoiceMeeterDesktop' --window-title 'DJMAX RESPECT V' `
  --wait 60 --duration 300
```

Testing showed that the current Discord desktop client did **not** include this
audio in DJMAX application-window sharing, even though Windows accepted the
HWND association. The stream is owned by `soundfork_discord_relay.exe`, while
application sharing filters for the selected DJMAX application. Entire-screen
sharing did include the relay audio. Use the validated mode instead:

```powershell
.\scripts\Start-SoundForkRelay.ps1
```

Share the entire monitor in Discord and keep the real microphone as Discord's
input device. The selected render endpoint must not be monitored back to the
FiiO output or the duplicate WASAPI stream will be audible as an echo.
The endpoint is a Phase 4 transport experiment and is not part of the low-latency
game playback path; SoundFork does not change the Windows default device.

The validated transport is base VB-CABLE; an installed VoiceMeeter endpoint is
also supported as a fallback. Neither is a SoundFork runtime dependency in the
core ASIO path. A standalone virtual Windows audio endpoint would require a
signed audio driver rather than only a user-mode program.

## License

SoundFork is licensed under GPL-3.0-only. See `LICENSE` and
`THIRD_PARTY_NOTICES.en.md`.

## CI/CD and releases

Pull requests and pushes to `main` build and test Debug and Release on Windows
x64. A release tag's base version must match `project(... VERSION ...)` in
`CMakeLists.txt`.

```powershell
# Prerelease for CMake VERSION 0.1.0
git tag -a v0.1.0-alpha.1 -m 'SoundFork v0.1.0-alpha.1'
git push origin v0.1.0-alpha.1

# Stable release
git tag -a v0.1.0 -m 'SoundFork v0.1.0'
git push origin v0.1.0
```

`alpha`, `beta`, and `rc` tags become GitHub prereleases. The Release workflow
publishes Setup.exe, a Windows x64 ZIP, and SHA-256 checksums only after the
installer/uninstaller smoke test passes. Build packages locally with:

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 0.1.0-alpha.1
```

Setup.exe requires Inno Setup 6. Pass `-SkipInstaller` to test only the ZIP.
