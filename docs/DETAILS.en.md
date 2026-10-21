# SoundFork details

[한국어](DETAILS.md) | **English** · [User guide](../README.en.md) · [Development guide](DEVELOPMENT.en.md)

This document explains operation, compatibility, latency and verification boundaries.
Use the README for installation and everyday use; use the development guide for
source changes, builds and test commands.

## Compatibility and scope

SoundFork is not a DJMAX- or FiiO-specific hooking tool. It opens a hardware driver
through the standard x64 ASIO COM interface and exposes SoundFork to the game or
DAW. Playback channels, sample rates and buffer sizes are queried from that driver.
The architecture can wrap other vendors' drivers, but compatibility with every
driver has not been tested. Real-use verification has been with DJMAX RESPECT V
and the FiiO K11 R2R.

Broadcast capture has additional constraints:

- Windows 10/11 x64 and x64 ASIO hosts.
- 48 kHz, stereo output channels 0 and 1, `ASIOSTInt32LSB` samples.
- Capture blocks of 1–8,192 frames; hardware must support the chosen buffer size.
- Only one prepared ASIO stream per process.
- Other formats, rates or channel layouts may play through ASIO without enabling capture.
- The dedicated OBS plugin targets OBS Studio 32.2.2 or later, x64.

64 samples is the tested FiiO setting, not a forced setting for every driver.
Install the manufacturer's driver first; SoundFork neither supplies nor modifies it.

## Audio paths and running components

```text
Game / DAW → SoundFork ASIO proxy → hardware ASIO driver → DAC
                         └→ audio copy → capture hub
                                            ├→ dedicated OBS source
                                            └→ relay → virtual render device
                                                           ├→ Discord entire-screen sharing
                                                           └→ OBS device capture
```

The game writes directly into hardware-driver-owned output buffers. SoundFork adds
no playback queue, playback worker or one-buffer-period wait. Broadcast data is
copied separately; a slow consumer loses capture blocks rather than delaying DAC output.

- `soundfork_driver.dll`: ASIO proxy loaded inside the game process, not a separate EXE.
- `soundfork_control.exe`: backend selection, status, relay and plugin management.
- `soundfork_capture_hub.exe`: distributes copied audio independently to OBS and the relay.
- `soundfork_discord_relay.exe`: renders broadcast audio to a virtual playback device.
- `soundfork-obs.dll`: audio source plugin loaded inside OBS.

The plugin or relay starts the hub automatically. It exits roughly two seconds
after both consumers close and consumers reconnect after game/hub restarts. The
diagnostic `soundfork_capture_monitor.exe` consumes the original ring exclusively;
do not run it alongside the hub.

Control is optional for playback. Closing its window while the relay runs hides
it in the tray; exiting Control alone need not stop the relay or ASIO playback.
Setup initializes the language preference and later user choices persist. Without
a saved preference, Korean Windows UI selects Korean; other languages fall back to English.

### Saved relay settings and sign-in startup

Selecting a VB-CABLE or Voicemeeter virtual playback device immediately saves it
for the current user. Stop the relay before changing devices; selection is locked
while it runs. **Settings → Start relay when signing in to Windows** is off by
default and requires explicit opt-in. Toggling it affects the next sign-in, not
the currently running relay.

Startup opens Control in the tray and launches the relay with only the saved
device ID. It waits for a late game or virtual device, without falling back to
default speakers or another same-name device. If reinstalling a device changes
its ID, stop the relay and select it again. Startup failures appear in status and
a tray notification, without endless relaunches. A manual stop lasts for the
session; the next sign-in starts again if the option remains enabled. Launching
Control again opens the existing window instead of another instance.

Registration requires no administrator rights and does not override a SoundFork
entry disabled in Windows Task Manager's **Startup apps**. Control shows whether
registration exists, not whether Windows separately blocked it; re-enable it in
Task Manager if needed. Updates preserve registration. Uninstall removes only
the removing account's entry for that exact installation. When uninstalling under
a different administrator account, disable startup in the original account first;
its registration may otherwise remain. Other user profiles are not modified.

## Latency and broadcast stability

### Game playback

The theoretical lower bound of SoundFork-added **buffering latency is 0 ms**.
That does not mean zero actual input-to-output latency or zero proxy CPU time.
The ASIO driver, USB, DAC and game contribute latency; copying also takes execution time.

At 48 kHz / 64 samples, a buffer period is about 1.33 ms by calculation. SoundFork
does not add a wait for that period, and the calculation is not a measurement.
Both direct and proxy paths reported 168 samples of output latency in the tested
environment. This is **driver-reported**, not a physical loopback measurement.

On 2026-10-19, a Release accelerated fake-backend test sampled every 4,096th callback
across 10,800,000 callbacks and recorded 0.121 us average and 7.7 us maximum callback
time. These are synthetic wall-clock results on that PC, not measured FiiO latency.

### Broadcast delay and isolation

Hardware ASIO and virtual WASAPI devices have independent clocks. Only the relay
path uses adaptive resampling, a four-block target queue and ±2,000 ppm correction.
At 48 kHz / 64 samples, four blocks represent about 5.33 ms by calculation, before
WASAPI and capture-app buffering. This queue and resampling do not affect game playback.

The OBS plugin delivers 480-frame / 10 ms packets and uses a residual queue and
clock correction. OBS buffering and video sync remain, so bypassing a virtual
device does not establish a measured recording-latency improvement. Compare recordings
and measure the result.

A full OBS queue does not hold up the relay or DAC. The relay is implemented to
reopen its WASAPI session after endpoint removal or an audio-service restart
without stopping ASIO. This does not promise uninterrupted playback if the actual
hardware ASIO driver or DAC itself disconnects.

## Discord and OBS behavior

ASIO bypasses the Windows shared audio engine. Direct FiiO ASIO output does not
appear in ordinary WASAPI loopback, and the relay's Windows audio stream belongs
to `soundfork_discord_relay.exe`, not the game.

Previous remote-listener tests found:

- DJMAX application-window sharing: no game audio, even with the relay.
- Entire-screen sharing with direct ASIO: no game audio.
- Entire-screen sharing with SoundFork relay and VB-CABLE: game audio included.

SoundFork therefore supports **entire-screen sharing** in Discord. A public
Windows 11 stream/window association API was also tried, but Discord still excluded
the external relay from application-window sharing. SoundFork uses no game injection
or private Discord APIs and does not treat routing music to the microphone as screen-share support.

The dedicated OBS source receives audio directly from the hub, without a virtual
device or relay. For simultaneous Discord use, run the relay and keep the dedicated
source in OBS. For device capture, select the same virtual playback device in
SoundFork and OBS: VB-CABLE's `CABLE Input` is playback; `CABLE Output` is recording.
Capturing the same audio through both OBS methods duplicates it.

Create one SoundFork source and reuse it across scenes. Multiple independent OBS
processes capturing directly are unsupported. Use plugin and relay components
from the same SoundFork build.

OBS-ASIO's `ASIO Input Capture` is a separate plugin, not SoundFork's output-copy
path. Reopening SoundFork or the game's hardware ASIO driver there to capture
game audio can cause device-ownership or sample-rate conflicts. Use the dedicated
`SoundFork Audio` source in OBS instead.

## Installation and plugin management behavior

Setup registers SoundFork using a selected registered x64 ASIO backend. It retains
a valid existing selection, otherwise preferring FiiO ASIO Driver when present.
With no compatible driver, it asks for the manufacturer's driver and stops. It
does not remove hardware drivers or virtual devices, or change Windows default devices.

The OBS plugin is optional at installation and can be installed, updated or removed
later in Control. Reading status and versions needs no elevation. An explicit file
operation requests administrator approval and refuses replacement/removal while
OBS is running. Volume and mute belong in OBS; Control has no separate delivery toggle.

The standard plugin location is `%ProgramData%\obs-studio\plugins\soundfork-obs`.
Setup keeps a private payload inside the SoundFork installation even when the OBS
task is unchecked. Portable OBS does not search ProgramData: manually place the
DLL and hub EXE in `obs-plugins\64bit`, and plugin `data` contents in
`data\obs-plugins\soundfork-obs`. Control manages the standard installation location.

## Verification records and outstanding checks

These are dated records, not guarantees for every driver, PC or future release.
See the [development guide](DEVELOPMENT.en.md#testing-and-diagnostics) for commands and reproduction steps.

- Hardware: FiiO ASIO Driver 6.16.0.0, FiiO K11 R2R, 48 kHz / 64 samples, stereo
  `ASIOSTInt32LSB`. The user confirmed stereo playback, no perceptible latency
  difference, no clicks/pops/dropouts and game restart. This is distinct from
  verification of full songs or extended uninterrupted playback.
- 2026-10-19: the registered COM path delivered 2,244 blocks / 143,616 frames with
  zero producer drops during a three-second consumption interval. Release Setup
  installation/removal and OBS Desktop Audio capture of `CABLE Input` were verified in real use.
- 2026-10-19 accelerated test: 10,800,000 callbacks represented four hours at the
  test rate, with ±1,500 ppm simulated relay drift. It recorded zero producer drops,
  zero consumer underflows, a one-block maximum queue and identical start/end
  private bytes. **This was not four hours of real-time hardware playback.**
- 2026-10-20 local regression: OBS-enabled Debug/Release CTest each passed 10/10;
  30 security-tool tests passed and one real-ClamAV test was skipped. Previous
  MSVC AddressSanitizer proxy/ring/relay and `/analyze /WX` passing records also exist.
- 2026-10-21 OBS fix: the user reported game-audio crackling/dropouts through the
  rc.2 dedicated source, while VB-CABLE output capture, microphone and browser
  audio were clean. An eight-second libobs waveform test reproduced ten sample
  discontinuities and five silence runs in the original code. Catching up missed
  delivery deadlines made the waveform test pass. OBS-only changes add at least
  20 ms residual headroom, underrun refill, MMCSS and reason-specific metrics.
  Virtual four-hour jitter/drift tests at ±1,500 ppm also passed. ASIO playback
  and WASAPI relay code remain unchanged; actual DJMAX recordings of the updated
  plugin and physical latency measurements are still pending.
  Debug/Release CTest each passed 12/12, timing/waveform/recovery checks each passed
  five repetitions, and `/analyze /WX` plus CFG/NX/ASLR/static-runtime checks passed.
  The normal waveform run had zero discontinuities, silence runs, underruns,
  discarded blocks, resyncs or OBS output drops. The intentional producer-pause
  test recovered after an underrun. A separate accelerated four-hour ASIO test
  also had zero producer drops or consumer underflows; it was not a hardware soak.
  Additional repetitions exposed occasional underruns, prompting bounded input
  retries of up to 20 ms on the OBS worker instead of immediately inserting zeros.
  This edge case reinforces that short passing runs do not guarantee long-duration
  audio quality on every PC.
  After this refinement, waveform/recovery tests including an intentional 30 ms
  delivery delay each passed five repetitions.
- 2026-10-21 relay persistence/startup changes: OBS-enabled Debug/Release CTest
  each passed 13/13, along with `/analyze /WX` and CFG/NX/ASLR/static-runtime checks.
  Security-tool tests passed 30 with one real-ClamAV skip. A separate accelerated
  four-hour-equivalent fake-ASIO run had zero producer drops or consumer underflows.
  The user's Run entry, installed copy and audio settings were not changed. Real
  sign-in/tray/late-device behavior and the full update/uninstall lifecycle still
  require manual or CI verification.
- Hub and real-libobs automation covers stereo PCM, increasing timestamps, slow
  consumer isolation, game/hub restart and simultaneous relay reception. It does
  not establish a real OBS UI recording result.

The following are not claimed complete:

- Observing nonzero DJMAX song PCM in the monitor and a four-hour real-time simultaneous soak.
- Physical virtual-WASAPI driver removal/reconnection.
- Simultaneous remote verification of a real microphone and Discord screen-share audio.
- FiiO/Discord/dedicated OBS plugin recording sync and long-duration stability.
- Physical loopback measurement of end-to-end or proxy-added latency.
- Real UAC approval/cancellation, full installer lifecycle for the new plugin manager,
  and actual symlink/junction rejection.

## Security coverage and trust boundaries

Shared-memory sizes, offsets and format are validated; callbacks use internally
cached sizes, and buffer pages are touched before streaming. This defends against
corrupt headers, not malicious processes with the same user privileges.

CodeQL analyzes C/C++ and Dependabot checks Actions dependency updates. Releases
containing the security workflow scan final Setup, ZIP and extracted files with
ClamAV and block publication on detection or scan failure. Existing published
releases are not automatically rescanned or replaced. See
[security operations](DEVELOPMENT.en.md#security-check-operations) for implementation and maintenance.

README badges report workflow execution, not a file safety certification or zero
vulnerabilities. One antivirus engine cannot guarantee malware absence; scanning
does not replace signing, SmartScreen reputation or hardware tests. Consult the
individual release's Actions history to establish which checks actually ran.

## References

- [Discord screen sharing](https://support.discord.com/hc/en-us/articles/360040816151-Go-Live-and-Screen-Share)
- [Windows process-loopback example](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- [Windows stream/window association API](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioviewmanagerservice)
- [License](../LICENSE) · [Third-party notices](../THIRD_PARTY_NOTICES.en.md)
