# SoundFork development guide

[한국어](DEVELOPMENT.md) | **English** · [User guide](../README.en.md) · [Program details](DETAILS.en.md)

For developers changing, building, testing and releasing SoundFork. Operation,
compatibility, latency figures and verification records belong in [program details](DETAILS.en.md).

## Prerequisites and quick start

- Windows 10/11 x64, Git and CMake 3.25 or newer.
- Visual Studio 2022 with **Desktop development with C++** and a Windows SDK.
- C++20 / MSVC x64 only; automated tests require neither a game nor a hardware ASIO driver.
- Inno Setup 6 for Setup.exe; Python 3.10+ for security-tool tests.

Run from the repository root in PowerShell. The build script also locates Visual
Studio's bundled CMake, then configures, builds and runs CTest.

```powershell
.\scripts\Build-SoundFork.ps1 -Configuration Debug -WithObsPlugin
.\scripts\Build-SoundFork.ps1 -Configuration Release -WithObsPlugin
```

Omit `-WithObsPlugin` for a build without the OBS plugin. OBS-enabled builds use
`Prepare-ObsSdk.ps1` to verify pinned SHA-256 hashes of official OBS 32.2.2 source
and Windows archives. A locally installed runtime of that version can be reused
for tests. Official headers and DLL exports generate the import library; OBS
runtime DLLs themselves must not enter SoundFork release packages.

Outputs are in `build/windows-msvc-x64/Debug` or `Release`: `soundfork_driver.dll`,
`soundfork_control.exe`, `soundfork_capture_hub.exe`, `soundfork_discord_relay.exe`,
optional `soundfork-obs.dll` and the `obs-plugin` payload. Do not move only the
Control EXE when testing plugin management.

## Finding the implementation

- `src/asio-proxy`: x64 ASIO COM server and registration entrypoints.
- `src/audio-bridge`: hardware loading/lifecycle, buffer and callback forwarding.
- `src/capture-backend`: shared rings, hub, WASAPI relay/clock correction and monitor.
- `src/obs-plugin`: official libobs source API integration and localization.
- `src/control-app`: settings/language/tray, update checks and elevated plugin management.
- `tests`: fake ASIO backend, regression, real-libobs integration and security tooling.
- `installer`, `scripts`, `.github`: installer, build/diagnostic scripts and CI/release workflows.

The base version comes from `project(VERSION ...)` in `CMakeLists.txt`; the
user-visible version is generated from `SOUNDFORK_VERSION`. Language preferences
are stored per user in `HKCU\Software\SoundFork\Language`.

## Contracts to preserve when changing code

### ASIO real-time path

`ProxyDriver::createBuffers` exposes the hardware driver's buffer pointers directly
to the host. Hardware callbacks pass through the proxy to host callbacks. Do not
insert a playback queue, buffer, worker or one-period wait for a broadcast feature.
Only one prepared stream is allowed per process; a second `createBuffers` returns
`ASE_InvalidMode`.

Do not add allocation, file/registry/network access, logging, locks, waits,
synchronous IPC, unnecessary conversion or broadcast-consumer waits in callbacks.
Keep the zero-allocation test over 10,000 callbacks including capture publication.
Clock correction and endpoint recovery belong only in broadcast paths.

### Shared memory and fan-out

- The input is `Local\SoundFork.Audio.v2`; OBS and relay outputs are
  `Local\SoundFork.CaptureHub.v1.OBS` and `.Relay`. Each ring is single-consumer SPSC.
- Prepare up to 64 blocks of 8,192 frames and touch roughly 4 MiB of data pages
  before streaming. Validate headers, mapping size, offsets and format checksums;
  cache validated copy sizes internally.
- Never re-trust shared header fields as callback lengths or offsets. Preserve
  Interlocked sequence/generation ordering and reject blocks racing with reset.
- The hub is the input's sole consumer and copies to both outputs in another
  process. Full outputs drop independently. Do not run the original monitor alongside it.
- A named mutex enforces one hub; kernel event-handle lifetimes track consumers.
  It exits roughly two seconds after both close. Generation/process checks every
  50 ms detect restarts; idle hub polling slows to 50 ms when no game is active.

Protocol changes require version/bounds validation and restart tests, including
same-format game restarts, forcibly terminated hubs and a stalled single consumer.

### OBS output and plugin management

The OBS worker sends 480-frame / 10 ms packets to `obs_source_output_audio` using
a continuous OBS monotonic timeline. Rearm a one-shot timer against accumulated
sample deadlines, not wake counts; catch up at most five packets per batch. Stalls
exceeding 50 ms or excessive queues resynchronize OBS only. Register the delivery
thread with MMCSS `Audio`, warning and continuing on failure. Keep process
launch/recovery on a separate supervisor thread.

OBS residual headroom is at least 20 ms or four blocks, whichever is greater:
960 frames at a 64-frame ASIO block size. Prefill an additional packet and
interpolation look-ahead, including after a real underrun. Correction remains
bounded to ±2,000 ppm; exclude pending catch-up packets from the drift estimate.
Accept only formats whose startup headroom is below the 48-block high-water mark
(48 kHz / 64 samples is supported). This is a broadcast buffer target, not measured
total latency; it does not change FiiO playback or the WASAPI relay.

Log attachment/disconnection and cumulative metrics every 30 seconds and at shutdown.
`idle_silence_frames` means waiting for a producer; `prebuffer_silence_frames` means
startup/recovery refill; `underrun_silence_frames` means missing data during delivery.
Separate `deadline_resyncs` from `queue_resyncs`; `discarded_blocks` counts blocks
discarded from the ring. `hub_output_drops` covers only the OBS output ring. Do not
attribute source-lifetime counters to one recording.

Management reads DLL version resources without loading the DLL. Only an explicit
operation launches the same Control EXE through `runas`. Fixed management commands
accept no arbitrary source/destination paths; derive the destination from the
ProgramData Known Folder. Refuse file operations while OBS or its capture client is active.

Reject links/reparse points and hold directory handles against path replacement.
Stage, back up and replace/remove only the seven known files; roll back earlier
changes on failure. Preserve unknown files and retain the transaction directory if
rollback fails. Whole-app uninstall also stops on plugin cleanup failure. User-facing
locations and portable behavior are in [program details](DETAILS.en.md).

## Testing and diagnostics

### Automated regression and analysis

The build script runs CTest: currently 12 tests with OBS, nine without it. Coverage
includes proxy/COM, rings/hub, relay clock, enumeration, Control, plugin management
and optional real-libobs integration. Libobs tests verify DLL loading, source
creation, continuous stereo waveforms, timestamp cadence, unexpected silence or
discontinuities, producer-pause recovery and simultaneous relay reception. The test
producer follows elapsed sample time. Separate virtual-clock tests cover late/missed
wakes, long stalls and four-hour-equivalent jitter with ±1,500 ppm drift. These are
not actual game/DAC tests.

```powershell
.\scripts\Test-SoundForkStaticAnalysis.ps1 -WithObsPlugin
.\scripts\Test-ReleaseDependencies.ps1
python -B -m unittest discover -s tests -p test_scan_release.py -v
```

Static analysis clean-builds in `build/windows-msvc-analyze` with `/analyze /WX`.
Release uses the static MSVC runtime. The dependency check verifies x64,
CFG/NX/ASLR and Guard CF metadata and absence of dynamic MSVC runtime dependencies.
Fix warning causes rather than suppressing them.

Tests use isolated `SOUNDFORK_CAPTURE_SCOPE` names and temporary directories. Do
not modify installed drivers, user plugins or audio settings to facilitate tests.
Directory symlink tests skip when Windows privileges are unavailable; real
symlink/junction and UAC approval/cancellation remain manual. Report skips as skips.

### Accelerated soak and hardware tests

```powershell
.\scripts\Test-SoundForkSoak.ps1 -Hours 4
```

This accelerates 10,800,000 callbacks representing four hours at 48 kHz / 64 samples,
plus ±1,500 ppm relay drift. Do not report it as four hours of real-time playback.
For hardware testing, the user starts the relay and runs the watcher:

```powershell
.\scripts\Watch-SoundForkHardwareSoak.ps1 -Hours 4
```

Record callback timing, producer drops, queue depth, under/overruns and memory.
A person must verify stereo sound, game restart, remote Discord audio/physical
microphone and OBS recording sync. Also exercise consumer stops and virtual-device
removal/reconnection without arbitrarily changing existing settings. Separate
calculations, driver reports, fake-backend timing and physical latency measurements.
Update evidence and outstanding checks in [program details](DETAILS.en.md#verification-records-and-outstanding-checks).

After OBS plugin changes, record the same song separately through the dedicated
source and `CABLE Input` output capture. Disable duplicate capture; include microphone
and browser audio, game exit and restart. Compare at least ten minutes, followed by
a multi-hour simultaneous soak checking sound, A/V sync and metric deltas. Distinguish
fixed sync error from increasing drift and record existing OBS offsets. Do not
automatically replace installed DLLs or treat automated passing results as an audio
quality guarantee before the user's recording checks.

## Development registration and installation

Registration changes the system ASIO list. Perform it only when needed from an
elevated x64 PowerShell session. FiiO is the example below; substitute another
backend's actual registered name as appropriate.

```powershell
.\scripts\Register-SoundFork.ps1 -BackendName 'FiiO ASIO Driver'
.\scripts\Get-SoundForkStatus.ps1 | Format-List
.\scripts\Unregister-SoundFork.ps1
```

For manual ZIP installation, run its `scripts/Install-SoundFork.ps1` elevated and
specify `-BackendName`. Never remove or modify manufacturer drivers. Automated
installer/COM/uninstaller checks use a temporary backend on isolated CI runners.
`Test-ReleaseInstaller.ps1 -AllowRegistryChanges` really writes the registry and
refuses existing SoundFork keys; do not run it casually on an in-use installation.

## Packaging and releases

### Local packages

With Inno Setup 6, packaging creates Setup.exe, ZIP and `SHA256SUMS.txt` in
`artifacts`. The version below is a local packaging example, not an instruction
to recreate an existing tag.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 1.0.0-rc.2
.\scripts\Build-ReleasePackage.ps1 -Version 1.0.0-rc.2 -SkipInstaller
```

With a certificate, the script SHA-256/RFC 3161 signs and verifies every shipped
EXE/DLL and Setup. Never commit certificate private keys or passwords.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 1.0.0-rc.2 `
  -CertificateThumbprint '<SHA1 thumbprint>' `
  -TimestampUrl 'https://timestamp-provider.example'
```

Edit `assets/soundfork-icon.png` and run `scripts/Build-Icon.ps1` to regenerate the
icon. When adding/moving documents, update CMake's install list and README links.
Both ZIP and Setup keep READMEs at the root and detail/development guides in `docs`.

### GitHub release procedure

1. Verify Debug/Release regression and static analysis, then commit the changes.
2. Choose an unused version. Base `X.Y.Z` must match CMake; optional suffixes are
   `alpha.N`, `beta.N` and `rc.N`. Match both READMEs' direct Setup links to it.
3. Land the release commit on remote `main`, then tag that commit and push the tag.
   These are intentional release-operator commands; never force-move an existing tag.

```powershell
$releaseVersion = Read-Host 'New release version without v'
git tag -a "v$releaseVersion" -m "SoundFork v$releaseVersion"
git push origin "v$releaseVersion"
```

PR/main CI runs Windows Debug/Release builds/CTest, Release static analysis and
security-tool tests. Tagged releases run **Windows build/existing checks → ClamAV
→ publish**. Optional signing precedes scanning. Scan and publish use the same
immutable artifact ID and build-recorded manifest SHA-256, without rebuilding or
re-signing. Reject moved tags before publication and serialize runs of the same
tag. Only publishing receives `contents: write`; failures never delete old releases.

Configure all of `WINDOWS_SIGNING_PFX_BASE64`, `WINDOWS_SIGNING_PFX_PASSWORD` and
`WINDOWS_TIMESTAMP_URL` for signing; otherwise an unsigned warning is emitted.
Suffix tags publish prereleases; plain version tags publish normal releases.

## Security-check operations

### CodeQL and dependencies

On main pushes, PRs, Tuesdays at 03:23 UTC and manual runs, CodeQL observes an
OBS-enabled Windows C/C++ build and uploads `security-extended` results to
Security → Code scanning. It is independent of ordinary CI/releases, and a
successful run does not mean zero alerts. PowerShell, Inno Setup, Python and
hardware behavior are outside this C/C++ analysis. Code scanning/Actions must be
available for the public repository. Switch any existing Default setup to Advanced
setup to avoid conflicts; settings are not changed automatically.

[Dependabot configuration](../.github/dependabot.yml) checks Actions dependencies
weekly. Keep full-SHA pins and manual review, not automatic merging. CMake and
manually downloaded SDKs are outside its scope; retain OBS archive hash checks.

### ClamAV and failure response

Ubuntu ClamAV packages and freshclam official signatures scan final Setup.exe,
ZIP and safely extracted payloads without running them. `scripts/scan_release.py`
validates the files/manifest against build-recorded hashes and rejects ZIP
traversal, links, encrypted entries and duplicate paths.

- 128 MiB per original/extracted file; 512 MiB total ZIP extraction/per-container expansion.
- 4,096 ZIP entries/container files; depth 32.
- 120 seconds per file, 15 minutes per scanner process, three-day maximum database age.

Detections, exceeded limits, warnings/errors, timeouts and refresh failures block
publication. No automatic exemption, bypass or infected-file deletion is allowed.
Inspect Actions logs and the 30-day `SoundFork-<version>-security-logs` artifact:
`freshclam.log`, `clamscan.log` and `scan-result.json` exist for reached stages and
record engine/database versions and target hashes. Investigate suspected false
positives before fixing/retrying.

Python tests use only the standard library; ordinary CI/local tests simulate
success, detection, errors and timeouts. Only the Linux release job enables
`SOUNDFORK_TEST_CLAMAV=1` for real ClamAV/EICAR tests in temporary directories.
Never commit or ship the sample. Establish actual uploads, signature refresh and
publication gating from Actions logs, separately from local tests. Coverage and
limitations are in [program details](DETAILS.en.md#security-coverage-and-trust-boundaries).

## Licensing and contributions

Check GPL-3.0-only and [third-party notices](../THIRD_PARTY_NOTICES.en.md). Do not
add non-redistributable SDK/runtime components or signing credentials. Review
playback-path changes separately from broadcast/UI work and state tested/untested
scenarios in pull requests.
