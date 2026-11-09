# SoundFork development guide

[한국어](DEVELOPMENT.md) | **English** · [User guide](../README.en.md) · [Program details](DETAILS.en.md)

For developers changing, building, testing and releasing SoundFork. Operation,
compatibility, latency figures and verification records belong in [program details](DETAILS.en.md).

## Prerequisites and quick start

- Windows 10/11 x64, Git and CMake 3.25 or newer.
- Visual Studio 2022 with **Desktop development with C++** and a Windows SDK.
- C++20 / MSVC x64, plus x86 ASIO components; automated tests need neither a game nor a hardware ASIO driver.
- Inno Setup 6 for Setup.exe; Python 3.10+ for security-tool tests.

Run from the repository root in PowerShell. The build script also locates Visual
Studio's bundled CMake, then configures, builds and runs CTest by default.
`-SkipTests` preserves compilation and skips only CTest execution for CodeQL;
normal CI and releases do not pass this switch.
The default build version is `1.3.0-dev`; `-Version` may override it for release builds.
Control checks asynchronously once per process on its first visible opening; hidden
sign-in startup waits until the window is opened. Automatic failures/up-to-date results
are silent. The opt-out is stored in `HKCU\Software\SoundFork\AutomaticUpdateNotifications`
(DWORD, default 1). Manual checks remain available. Tests use fake checks, not GitHub requests.
The alpha version in packaging examples is for explicitly preparing a prerelease,
not an instruction to publish automatically.

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
optional in-game relay `soundfork_app_relay.dll`, `soundfork_control.exe`,
`soundfork_capture_hub.exe`, `soundfork_discord_relay.exe`,
optional `soundfork-obs.dll` and the `obs-plugin` payload. Do not move only the
Control EXE when testing plugin management.

## Finding the implementation

- `src/asio-proxy`: x64/x86 ASIO COM servers and registration entrypoints.
- `src/audio-bridge`: hardware loading/lifecycle, buffer and callback forwarding.
- `src/capture-backend`: shared rings, hub, WASAPI relay/clock correction and monitor.
- `src/obs-plugin`: official libobs source API integration and localization.
- `src/control-app`: settings/language/tray, update checks and elevated plugin management.
- `tests`: fake ASIO backend, regression, real-libobs integration and security tooling.
- `installer`, `scripts`, `.github`: installer, build/diagnostic scripts and CI/release workflows.

The base version comes from `project(VERSION ...)` in `CMakeLists.txt`; the
user-visible version is generated from `SOUNDFORK_VERSION`. Language preferences
are stored per user in `HKCU\Software\SoundFork\Language`.

## 1.3.0 Windows playback and 32-bit ASIO hosts

### Windows playback backend

`windows_audio_driver.*` implements IASIO; `windows_render_device.cpp` provides 48 kHz
stereo Float32 shared-mode WASAPI. Both architectures read the exact device ID from the
64-bit view of `HKLM\SOFTWARE\SoundFork\Output\WindowsEndpoint`. No value selects vendor ASIO.
Setup's `windows` choice snapshots the current default's ID, or saves `unconfigured` when
there is no device. It does not change the default or follow later changes. Select an actual
device in Control. Silent Setup accepts `/BACKENDCLSID=windows`.

Buffer preparation allocates/prefaults samples and creates events and a COM worker. Lifecycle
start/stop acknowledgments may wait; calls from callbacks are rejected. WASAPI events determine
available render space. A partial-block offset bridges device and ASIO periods without skipping
or repeating samples. Device-clock/QPC values become ASIO frames/nanoseconds. Reported latency
uses the stream latency/buffer and ASIO block; it is not a physical measurement. A 64-frame ASIO
block does not imply a 64-frame Windows engine period. Recoverable device failures reopen
the same selected endpoint; other render failures request an ASIO reset and stop rendering.
There is no automatic switch to another/default endpoint or gap-free reconnection guarantee.

Windows output now queries `IAudioClient3::GetSharedModeEnginePeriod` and requests the
smallest valid period through `InitializeSharedAudioStream` (EVENTCALLBACK only).
This requires support for the selected stream format (native mix when supported). Unsupported
interfaces/formats, invalid period metadata or initialization failures activate a fresh
audio client and fall back to ordinary shared initialization. No exclusive
mode or kernel streaming is used. Vendor ASIO callbacks remain unchanged.
On this path the queue target is two engine periods, capped at endpoint capacity,
retaining a scheduling margin instead of filling the entire buffer. Legacy fallback
retains full-capacity filling. The test host prints period, capacity, queue target and
low-latency HRESULT after preparation; these are settings, not physical measurements.
Tests cover period bounds/alignment, refill limits and negotiation/fallback failures.

Windows output already owns an in-process WASAPI session, so the additional internal relay is
skipped to avoid duplicate capture. Vendor ASIO callback code remains unchanged. Fake-device
tests cover 64/256/2048 frames, both callback types, partial periods/stereo waveforms, clock,
restart/failure and slow-consumer isolation. Real listening, Discord sharing and long sessions
still need manual verification. A registry-free silent probe is available:

```powershell
.\build\windows-msvc-x64\Release\soundfork_host.exe --windows-endpoint @default --duration 2 --signal silence
```

API references: [IAudioClient3::InitializeSharedAudioStream](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream),
[IAudioClient::Initialize](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize),
[IAudioClock::GetPosition](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition).

### Native Windows mix-format conversion

Windows playback reads `GetMixFormat()` at buffer preparation. Stereo Float32 mix
formats from 32 to 384 kHz use their native sample rate for shared initialization
and engine-period negotiation. Game ASIO and capture remain 48 kHz stereo. Other
mix layouts/formats retain the previous 48 kHz shared automatic-conversion fallback.
An unsupported low-latency interface/period falls back to ordinary shared rendering
with the selected native format; no format/default setting is changed.

When rates differ, a causal 64-tap polyphase windowed-sinc converter runs only in
the Windows render worker after host callbacks. Coefficients/staging are allocated
and touched before playback; processing allocates nothing. Equal rates bypass it.
Rational phase/history persist across render events. Queue/period diagnostics use
device-rate frames; ASIO latency/capacity/clock remain in 48 kHz source frames. The
reported conversion allowance is 32 source frames (about 0.67 ms), not a physical
measurement. Native vendor-ASIO callbacks are unchanged. Tests cover rational
chunk equivalence, stereo, reset, DC gain and a downsampling rejection case.

Live format changes/device removal use session-disconnection notifications and WASAPI
errors. The worker releases the failed stream and retries the exact selected ID every
500 ms, reading mix format and preparing conversion again. During the outage it calls
the host on a QPC-paced fallback clock and discards playback samples instead of saving
them for later. Catch-up is bounded to four callbacks per worker iteration. After reopen,
the source clock stays monotonic and partial old output is dropped. Stop cancels retries;
dispose interrupts the wait and joins the worker. Native-ASIO callbacks remain unchanged.
Reopening/COM allocation occurs outside host callbacks, on the Windows worker. OS device
initialization is synchronous and may briefly delay fallback callbacks and stop/shutdown
acknowledgments; this is not a hard real-time or no-game-stall guarantee. Audio gaps and
filter startup transients are possible. Recovery counters/current device rate are atomic;
`output_info()` remains the initial preparation snapshot. The test host prints recovery
totals on stop. Stereo Float32
32/44.1/48/96/192/384 kHz conversion is unit-tested; broader music quality, unusual
device formats and long-session behavior still need validation.

Recovery tests use a fake device for 96->192->48 kHz reopen, repeated absence,
continued callbacks, sample-position monotonicity, stop/invalidation races, restart
while missing and disposal during recovery. An opt-in real-stream test is
`soundfork_windows_audio_tests --real-reopen <render-ID>`: it injects one invalid-device
error after a second of silent playback, then reopens the actual selected WASAPI
endpoint. It does not change the Windows format or physically disconnect hardware.
Real format-switch notifications, unplug/replug and game behavior remain manual tests.

### Optional manual software-loopback probe

Configure `-DSOUNDFORK_BUILD_LATENCY_PROBE=ON` and build `soundfork_latency_probe`.
It defaults OFF, has no CTest entry and is not installed. `--list` lists render/capture
IDs and states. Run `soundfork <render-ID> <block-frames>`,
`dll <render-ID> <frames> <absolute-DLL-path> <CLSID>` (registration-free loading), or
`registered <render-ID> <frames> <CLSID>` (existing ASIO registration). Third-party
output configuration must actually route to the selected capture endpoint.

The callback timestamps PCM writing and emits 20 small 48 kHz bursts. Float32 WASAPI
loopback records sample QPC timestamps and packet-arrival QPC separately. Count mismatch,
ASIO reset, erroneous/discontinuous marker timestamps or ambiguous timing fail the run.
Initial silent-packet discontinuities are counted separately. Use a quiet endpoint.
Arrival includes capture delivery/batching; sample timestamps belong to the API timeline.
Neither is DAC, physical output, roundtrip or touch-to-sound latency. The probe does not
change defaults/rates or register drivers. Exclusive ASIO/KS paths require physical
loopback for a comparable output measurement, not ordinary WASAPI loopback.

### x86 packaging and verification

The OS remains Windows x64. The proxy, internal relay DLL, registration helper and
test host also build as x86 into `bin/x86` (`{app}/x86` after installation). Control,
the hub, external relay and OBS plugin remain x64. The internal x86 relay launches
the x64 hub from the parent installation directory. Control matches the selected
x64 vendor's CLSID or ASIO registry name in the x86 registry view. With no match,
the x86 proxy is registered without a backend; x64 playback still works. Install
the matching vendor's x86 driver. No unrelated driver is selected or cross-loaded.

```powershell
.\scripts\Build-SoundFork.ps1 -Configuration Release -WithObsPlugin
.\scripts\Build-SoundFork.ps1 -Configuration Release -Architecture x86
.\scripts\Test-SoundForkCrossBitness.ps1 -Configuration Release
.\scripts\Test-SoundForkStaticAnalysis.ps1 -Architecture x86
```

Use `-Configuration Debug` for Debug coverage. Per-architecture CTest runs independently;
the cross-bitness script needs both builds and runs as a required CI/package step.
It checks a real x86 fake-host proxy -> x64 hub -> two consumers, and the reverse
producer/consumer bitness, across eight representative formats and 64/256 frames.
`-AllFormats` expands this to all 18 PCM types. Acknowledgment waits
are outside callbacks in the fake host. Wire v3 and its sizes/offsets are unchanged
and asserted at compilation. Counters use aligned Interlocked64 operations; read-only
diagnostic snapshots use SSE2 MOVQ. Registration tests redirect HKLM only in the test
process to a temporary HKCU key. Full dual-view install/update/uninstall runs in isolated
release CI. Real x86 games/vendor drivers, Discord sharing and physical latency remain
manual checks.

## 1.2.0 application-window relay

`RelayEnabled` (DWORD, default 1) gates optional DLL loading and Control's external
relay launch. Disabling stops forwarding and preserves mode/startup preferences;
OBS remains independent. `RelayBufferFrames` (DWORD, default 0) sets the shared
relay queue target. Zero preserves four ASIO blocks. The UI displays frames and
their duration using the actual stream sample rate. Before stream metadata is
available, Auto displays "waiting for ASIO" and manual choices show frames only.
Choices span 128–9600 frames; the effective target is
bounded to 1–32 ASIO blocks to preserve ring headroom. These are queue targets,
not measured end-to-end latency. Changes stop forwarding; restart external
forwarding or restart the game for application mode.

Control stores `entire-screen` or `application` in
`HKCU\Software\SoundFork\RelayMode`. Only the real COM-created `ProxyDriver` permits
the optional loader; fake-backend unit tests leave it disabled. Application mode reads
settings and explicitly loads adjacent `soundfork_app_relay.dll` from the non-real-time
`createBuffers` lifecycle path.

The user's sign-in choice is stored separately as the `StartupPreference` DWORD. If it
is absent on an existing installation, the current SoundFork Run registration is migrated
as the initial preference. Application-sharing mode preserves the checked state while
removing only the effective Run entry; returning to entire-screen mode restores that entry.
This avoids launching Control unnecessarily at sign-in for the in-game relay.

The DLL runs the existing `soundfork_relay_main` on a worker and consumes the hub's relay
output. It adds no second copy, branch, lock, wait or WASAPI call to the ASIO callback.
`disposeBuffers` and driver destruction stop the worker before unloading the DLL. Internal
and external relays share `SoundFork.DiscordRelay.Singleton.v1` and the same stop event.

Automated coverage checks setting defaults/persistence, DLL exports and its disabled
lifecycle plus existing relay, hub and ASIO regressions. Discord attribution of an
application-owned WASAPI stream is not a public contract, so each release still needs a
real application restart and remote-listening check.

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

- The input is `Local\SoundFork.Audio.v3`; OBS and relay outputs are
  `Local\SoundFork.CaptureHub.v2.OBS` and `.Relay`. Each ring is single-consumer SPSC.
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

On a brief input shortage, retry with a one-shot timer every 1 ms for at most
20 ms, preserving packet timestamps instead of immediately inserting zeros.
Catch up when input arrives; after the bound expires, count an underrun and refill.
Never wait on the ASIO producer or busy-spin. This permits transient broadcast
packet arrival delay; it is not a zero-latency guarantee.

Log attachment/disconnection and cumulative metrics every 30 seconds and at shutdown.
`idle_silence_frames` means waiting for a producer; `prebuffer_silence_frames` means
startup/recovery refill; `underrun_silence_frames` means missing data during delivery.
Separate `deadline_resyncs` from `queue_resyncs`; `discarded_blocks` counts blocks
discarded from the ring. `hub_output_drops` covers only the OBS output ring. Do not
attribute source-lifetime counters to one recording.
`input_wait_wakes` counts bounded input retries, not audio drops.
`max_lateness_us` includes input retries relative to packet deadlines; it is not
ASIO callback execution time.

Management reads DLL version resources without loading the DLL. Only an explicit
operation launches the same Control EXE through `runas`. Fixed management commands
accept no arbitrary source/destination paths; derive the destination from the
ProgramData Known Folder. Refuse file operations while OBS or its capture client is active.

Reject links/reparse points and hold directory handles against path replacement.
Stage, back up and replace/remove only the seven known files; roll back earlier
changes on failure. Preserve unknown files and retain the transaction directory if
rollback fails. Whole-app uninstall also stops on plugin cleanup failure. User-facing
locations and portable behavior are in [program details](DETAILS.en.md).

### User settings and sign-in startup

`control-app/relay_settings.*` manages `HKCU\Software\SoundFork\RelayEndpointId`
and the `SoundFork` value under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. The Run value is the
startup preference: a quoted absolute `soundfork_control.exe` path plus
`--startup`. Commands longer than 260 characters and malformed REG_SZ data are
rejected. Foreign commands are not overwritten; disable/removal deletes only an
entry matching the current executable. Windows StartupApproved internals are not
read or modified.

`--startup` exits quietly without registration; otherwise it starts Control in
the tray and the persistent relay. A session-local mutex makes the GUI single
instance; a normal relaunch shows the existing window. `--self-test`, plugin
management and `--remove-user-startup` bypass that GUI mutex. The last command
cleans only the current account's registration for the current EXE during removal,
never other users' profiles. ZIP removal checks the version resource capability
`SoundFork.StartupManagement/1` before invoking it.

Control passes `--endpoint-id <opaque-id>` to the relay. An absent exact ID waits
for reconnection, without substring-name matching or a default-output fallback.
The existing manual CLI `--endpoint <name-part>` search remains available. Audio
rendering, drift correction and the ASIO playback path are unchanged.

## Testing and diagnostics

### Automated regression and analysis

The build script runs CTest by default: currently 28 tests with OBS, 21 without it. Coverage
includes proxy/COM, rings/hub, relay clock, enumeration, Control, plugin management
and optional real-libobs integration. Libobs tests verify DLL loading, source
creation, continuous stereo waveforms, timestamp cadence, unexpected silence or
discontinuities, producer-pause recovery and simultaneous relay reception. The test
producer follows elapsed sample time. Separate virtual-clock tests cover late/missed
wakes, long stalls and four-hour-equivalent jitter with ±1,500 ppm drift. These are
not actual game/DAC tests.
The test producer also uses MMCSS `Audio` and reports maximum publication and
observed hub-delivery intervals.
A deliberately delayed 30 ms PCM batch is still subject to silence, waveform and
timestamp assertions, with no exclusion window.

### Coverage and execution cost

| Area | Automated evidence | Not established |
| --- | --- | --- |
| Proxy/decoder/ring | Buffer identity, callbacks/publication counts, four raw formats, boundaries/header corruption, C++ `new/new[]` interception | Real vendor ASIO behavior, physical latency, exhaustive CRT/Win32 allocation interception |
| Hub/accelerated soak | Fan-out, stalled-client isolation, restart/crash recovery, repeated PCM/counters/queues/memory observations | Real elapsed hours, hardware clocks, WASAPI device failures, proof of no leaks from memory reports alone |
| Real libobs/virtual clocks | Four waveforms, stereo, PTS, silence/jitter, integer/float recovery and simulated drift | Actual OBS video/encoder, remote Discord listening, physical DAC sync |
| Control/settings | Hidden real Win32 UI, private HKCU settings/permissions, singleton and disabled-startup exit | Real sign-in/UAC/clipboard, every DPI/device setup |
| Security | CodeQL/MSVC analysis, scanner error/path/checksum/publication policy, release ClamAV gate | Guarantee of no vulnerabilities/malware, signing, actual antivirus scanning from ordinary CTest alone |
| Deployment | CTest selection-policy/process-timeout/DLL checks; isolated release-CI install/upgrade/uninstall | Actual in-use-PC installation/UAC; local policy tests do not prove installation succeeded |

Keep Debug/Release, MSVC analysis and CodeQL: they serve different purposes. CodeQL
compiles test sources but leaves execution to ordinary CI/releases. Release CTest
binds testing to the final version's build and is retained. Text-based workflow
policy tests do not execute GitHub Actions. The Windows CI job has a 30-minute limit.

Keep each roughly eight-second real-libobs test: 1.5-second settling, delayed input
at four seconds, and over 48,000 continuous checked frames need this window.
Integer/float recovery tests exercise distinct renderers. Separate settings,
singleton and disabled-startup cases are cheap and localize failures. Only the raw
hub matrix reuses one process across four format changes, removing repeated
two-second idle exits; stalled-client/crash/auto-exit checks remain. The build script
already builds all default targets, so only the redundant subsequent three-target
build invocation was removed.

After 128 warm-up callbacks, `proxy_tests` separately measures 10,000 normal copies
and 10,000 full-ring drops. Normal-copy consumption/PCM validation is outside the
timed region; publication/consumption counts must advance with zero drops. The drop
fixture prefills 64 slots and requires no new publications and exactly 10,000 drops.
Each reports mean/p50/p95/p99/max. Timing includes fake-host PCM writes and the proxy,
not consumer processing. Functional counters/allocation checks are gates, not noisy
CI wall-time thresholds. These are not physical input/output latency measurements.

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

Settings tests use only GUID-named temporary HKCU subkeys. They cover off-by-default
startup, saved devices, quoted space/Unicode paths, foreign-value preservation,
malformed data, DACL access denial, cross-process single-instance behavior and
disabled `--startup` early exit. Device-ID matching is tested without opening
audio devices. CTest separates settings/permissions (`soundfork_relay_settings_tests`,
20 seconds), singleton behavior (`soundfork_control_instance_tests`, 30 seconds)
and disabled startup (`soundfork_disabled_startup_tests`, 30 seconds). Immediate
progress includes UTC start times, phase durations, child PIDs, wait results and
exit codes. Each child gets 10 seconds, followed on timeout by termination of
that exact child and at most two seconds to reap it. An intentionally delayed
child verifies termination/cleanup. Use
`ctest --test-dir build/windows-msvc-x64 -C Release -R 'soundfork_(relay_settings|control_instance|disabled_startup)_tests' -V`
for verbose progress. Normal CI/releases preserve `LastTest.log` and
`LastTestsFailed.log` for 30 days in `*-test-logs` artifacts, including on failure.
Test failures still block builds/publication. Real sign-out/sign-in, tray relaunch, late devices, manual-stop
persistence, Windows Startup-app blocking and alternate-administrator uninstall
still need manual verification. The isolated runner's installer test checks
startup registration survives an update and is removed on uninstall. Do not run
installation/registration tests on a working audio setup.

### Accelerated soak and hardware tests

`control_status_tests` covers Korean/English summaries for missing configuration,
late devices, input waiting, stopping and errors, without claiming successful
delivery from process existence. `control_ui_tests --test` creates hidden real
Win32 controls and verifies diagnostics creation, language refresh, close and
recreation. Without arguments it opens a read-only UI preview. It does not use the
installed GUI mutex or tray; preference writes, online actions and audio start/stop
commands are blocked. Actual copying is excluded to preserve the user's clipboard.
Other DPI settings, clipboard success/contention and real sign-in remain manual checks.
The main checkbox is checked for an off default, preference refresh, translations
and absence from the menu. `installer_policy_tests.ps1` only reads files to verify
there is no vendor/first-driver fallback and that install/register scripts require
`BackendName`. Isolated installer CI also checks rejected fresh unattended setup
without selection, startup off on fresh install, and preserved choices on upgrade.
Installer notices use `SuppressibleMsgBox` to honor unattended `/SUPPRESSMSGBOXES`.
`release_process_tests.ps1` checks exit-code propagation and termination of the
test's process tree on timeout. Install/upgrade/uninstall each have a 120-second
limit, the control self-test has 30 seconds, and the CI step has 12 minutes.
Setup/uninstall logs survive failure in `SoundFork-<version>-installer-logs` for
30 days. To test the real initialization rejection paths without elevation or
changing an installation, run:

```powershell
.\tests\installer_rejection_tests.ps1 -CompilerPath 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
```

This extracts the production `InitializeSetup` function and compiles it with
synthetic backend discovery. The probe never proceeds to installation.

```powershell
.\scripts\Test-SoundForkSoak.ps1 -Hours 4 -Format all -BufferSize 64 -SkipBuild
```

The original soak was a manual accelerated tool with a four-hour default and no
`add_test` registration (introduced in `e6b4838`). No explicit exclusion rationale
was recorded. Keep the long profile manual; the new `soundfork_soak_ci_tests`
executes 30 simulated seconds per format, all 18 formats, with a 60-second timeout
in ordinary Debug/Release and release CTest. No extra workflow repeats that test.
Run `soundfork_soak_tests --seconds 30 --format all` directly for the short profile.
`--hours`/`--seconds` specify simulated duration per format, not real elapsed time.
Each block verifies raw PCM, callback/publication/consumption counts, drops,
underflows and queue depth, while reporting private bytes. The Int32 profile retains
the original consumer read/decoder path; other formats are verified as raw bytes.
The fake driver uses
preallocated native-width buffers and configurable blocks. Proxy tests exercise
18 formats × 32/64/256/4096 frames; soak accepts other sizes via `--buffer-size`
(Float64 is limited to 4096).
Without `-SkipBuild`, the script builds and runs full CTest; add `-WithObsPlugin`
to keep OBS enabled. Relay drift tests are repeated only with `-SkipBuild`, when
CTest has not already run them. The command above accelerates 10,800,000 callbacks
per format plus ±1,500 ppm relay drift. Do not report it as real-time playback.
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

### Diagnostic logs

Available starting with v1.0.1; existing installations need an update.
Open **Help > Open log folder** or
`%LOCALAPPDATA%\SoundFork\Logs`. Each of `control.log`, `relay.log`, `hub.log`
retains a current file and `.1`/`.2` backups, capped at 1 MiB each (9 MiB total
under normal self-generated logging). Rotation replaces older history. Logs
remain as user diagnostic data after uninstallation. Records
contain UTC (`Z`), PID, version and structured events, not PCM, microphone audio,
endpoint names/IDs or shared-window titles. Paths can contain the Windows user
name; review before sharing. OBS source statistics remain in OBS's existing log.

Control logs launch origin, forwarding requests/spawns/stop signals, observed
state changes and startup-preference errors. A spawn or signal is not proof of
audible playback or completed shutdown. Relay logs connection/wait/restart,
HRESULTs, exit codes and 30-second/session-end metrics. `relay_audio_gap` reports
observed underflow/resync at most once per second. Initial `prebuffer_frames`
are separate from real `underflow_frames`. Hub logs source changes and ASIO/hub
drops every 30 seconds. Stream-local counters can reset on reconnection; session
restarts do not necessarily mean a physical endpoint disconnect.

`soundfork_diagnostics` is not linked into the ASIO driver/bridge. Only UI/capture
workers enqueue into a fixed 128-record queue using `try_lock`; a separate writer
performs file IO and rotation. Contention/full queues drop logs, reported by
`logger_dropped`. IO errors back off and never propagate into audio failures.
Drain/join happens only at process shutdown, never in ASIO callbacks. Crashes,
forced exits and disk failures can lose the tail; missing logs do not prove an
absence of faults. Diagnostics' logger counters describe the control writer.

`diagnostics_tests` uses private temporary directories to test contents, newline
sanitization, rotation, restart, concurrent producer congestion, invalid paths and
recovery after file contention. Hardlinked log files are also tested for rejection
without modifying their target. Opening the real GUI folder and correlating
start/stop/device reconnection in a new build remain manual checks.

## 1.1.0 raw PCM transport and consumer conversion

### Formats and playback invariant

v1.3.0-dev keeps wire IDs 1-4, mapping size and 64 KiB slot size, adding IDs 5-18.
Float64 capture is capped at 4096 frames; oversized blocks keep ASIO playback and
disable capture with a buffer-size diagnostic. Producer preparation and header
validation both enforce the payload limit. Older consumers reject unknown IDs:
update the hub/relay/OBS components together for new formats. Decoding is consumer-only.
Float64 NaN/Inf becomes silence; finite values outside Float32 range saturate at
Float32 limits. OBS float sources avoid Int32; the WASAPI relay retains its existing
Int32 consumer path. Valid-bit integer containers use sign-extended low bits, ignoring
upper padding. The mapping does not grow for existing FiiO/Int32 streams.

Capture supports 32/44.1/48/88.2/96/176.4/192/352.8/384 kHz stereo capture of two selected outputs: Int16/Int24/Int32/Float32/Float64
in LSB/MSB order and right-aligned 16/18/20/24 valid-bit Int32 containers (18 types).
DSD, multichannel and rates outside this list remain excluded.
Unsupported/mismatched formats disable capture, not playback, with a diagnostic
reason. This is not a guarantee of compatibility with every ASIO driver.

Validate format and cache lengths during `createBuffers`. Preserve backend buffer
pointers, callback/`outputReady` ordering and publication points. The ASIO callback
performs one `memcpy` per channel, without conversion, resampling, a new waiting
queue or logging. A rate notification differing from the current stream or reset request disables capture
atomically; only safe buffer recreation reopens it. Silent driver format changes
without notifications are not detected by querying the driver in callbacks.

### v3 transport contract

`HKCU\Software\SoundFork\CaptureChannelPair` stores the zero-based left/right indices in the low/high 16 bits of one DWORD. Both must be 0–63; invalid values fall back to 0/1. UI numbers are 1–64. Read once at COM creation, cache, and restart the game/DAW to apply. `createBuffers` resolves selected output `channelNum` values among the application's opened buffers, excluding inputs. Identical selections duplicate mono; reversed selections swap sides. Missing channels or mismatched PCM formats disable capture only. Callbacks retain two copies with no preference reads, searches or conversion. Hub/OBS/relay still receive stereo; no wire changes. Fake 8/64-channel tests cover reordered non-contiguous buffers, swap/mono/missing/mismatched formats and allocation-free callbacks. Real multichannel interfaces remain manual validation.

Store the original rate without resizing mappings. Relay opens a source-rate Int32 WASAPI shared stream; Windows converts to the endpoint rate. OBS receives source-rate 10ms packets and converts to the project rate. Existing consumer interpolation handles clock drift. OBS buffering scales to at least 20ms. Keep the existing <=48kHz queue policy; higher rates require a ring holding over 60ms for jitter recovery (e.g. 96kHz/128 frames or 384kHz/512 frames). Smaller blocks retain ASIO playback, raw capture and relay; only OBS attachment is rejected. Windows playback ASIO input remains 48kHz. Recreate buffers to reconnect consumers at a new rate. Update all components together: older consumers reject non-48kHz streams.

Automated coverage: 9 rates x 18 formats x 4 block sizes for raw byte identity and allocation-free callbacks; ring rate validation; real libobs 44.1/96kHz input to a 48kHz project. Non-48kHz DAC playback, Discord reception and extended hardware soak remain manual. No physical latency measurement is implied.

- Input: `Local\SoundFork.Audio.v3`; outputs: `Local\SoundFork.CaptureHub.v2.OBS` and
  `.Relay`. The 18 independent wire IDs use planar
  `[all left samples][all right samples]` storage.
- Headers describe format, layout, sample width, channels, rate, frames and payload
  bytes. Validate allowed values, bounds, checksum, consistent lengths and generation;
  copy using cached lengths only. The right plane starts halfway through the payload.
- Retain 64 slots and two channels: up to 8192 frames, or 4096 with eight-byte samples.
  Prefault the roughly 4 MiB data region before streaming; each slot is at most
  64 KiB. Capacity is not actual queued latency.
- The hub forwards original payloads. Consumer-side ring APIs perform conversion:
  `read_planar` copies bytes; `read_int32_stereo` and `read_float_stereo` decode on workers.
- Disabled-capture diagnostics use a mapping with ready cleared. Its format fields
  are placeholders, not the device format. Readers cannot attach and Control does
  not display those fields as an active audio format.
- No dual v2 publication or fallback. Update driver/hub/relay/OBS together.
  Control warns about a legacy input mapping or an installed plugin version that
  differs from its included payload.

### Convert only where required

Relay workers scale S16 by 65536 and sign-extended S24 by 256; S32 values remain
unchanged. F32→Int32 maps NaN/Inf to zero, clamps to [-1,1], multiplies by 2³¹,
rounds nearest with ties away from zero, and saturates to Int32 limits.
No signed-negative left shifts. WASAPI and drift policies remain unchanged.

OBS keeps its Int32 source/interpolation/output path for integer inputs. F32 uses
float source/interpolation/output, preserving finite values and zeroing NaN/Inf
without an Int32 round trip. Timestamp, jitter-buffer, high-water resync, drift and
target-frame policies remain unchanged.

### Automated coverage and manual gates

Tests cover decoder boundaries, signed packed Int24, unaligned/nonfinite inputs,
saturation, all-format byte identity, allocation-free callbacks, stalled consumers,
publication count, corrupt headers, generations and raw hub fan-out. Consumer
interpolation tests include normalized integers and floats below Int32 resolution.
Real-libobs tests cover all four waveforms/stereo/timestamps/jitter and integer/float
pause recovery.

Physical FiiO 48k/64, multi-hour Discord/OBS use and real alternate-format drivers
remain manual checks. Synthetic callback timings and accelerated drift simulation
are neither physical latency nor real-time soak measurements. Building sources does not replace an installed copy.

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
specify `-BackendName`. Both install and registration scripts require this argument;
there is no vendor default. Never remove or modify manufacturer drivers. Automated
installer/COM/uninstaller checks use a temporary backend on isolated CI runners.
`Test-ReleaseInstaller.ps1 -AllowRegistryChanges` really writes the registry and
refuses existing SoundFork keys; do not run it casually on an in-use installation.

## Packaging and releases

### Local packages

With Inno Setup 6, packaging creates Setup.exe, ZIP and `SHA256SUMS.txt` in
`artifacts`. The version below is a local packaging example, not an instruction
to recreate an existing tag.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 1.1.0-alpha.1
.\scripts\Build-ReleasePackage.ps1 -Version 1.1.0-alpha.1 -SkipInstaller
```

With a certificate, the script SHA-256/RFC 3161 signs and verifies every shipped
EXE/DLL and Setup. Never commit certificate private keys or passwords.

```powershell
.\scripts\Build-ReleasePackage.ps1 -Version 1.1.0-alpha.1 `
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

CodeQL passes `-SkipTests`: it still compiles test sources, but does not duplicate
child-process execution tests under the process tracer. Full execution tests
remain in normal Debug/Release CI and releases. Build-policy tests verify that
only CodeQL receives this switch and that failure-log retention is preserved.

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
