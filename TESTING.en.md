# Testing

[한국어](TESTING.md) | **English**

## Automated Phase 1 tests

`soundfork_tests` uses an in-process deterministic ASIO test driver exposing two
48 kHz `ASIOSTInt32LSB` outputs with a fixed 64-sample buffer. It verifies:

- COM-style `QueryInterface`, reference counting, and instance cleanup;
- capability and lifecycle forwarding;
- exact pointer identity between host and downstream output buffers;
- left/right sample integrity over 10,000 callbacks;
- legacy and time-info callback forwarding;
- `outputReady` forwarding;
- zero dynamic allocations in the callback path;
- rejection of a second active stream.

`soundfork_com_tests` loads the built DLL without registration, verifies all COM
exports, creates the class factory and `IASIO` object using SoundFork's CLSID,
and confirms `DllCanUnloadNow` lifetime behavior.

CTest also runs `soundfork_host --list`, which is read-only.

`soundfork_capture_tests` verifies shared-memory format negotiation, exact
stereo interleaving, exclusive producer/consumer ownership, bounded capacity,
and drop-on-full behavior. The proxy test additionally reads captured samples
written by the fake ASIO host and keeps allocation tracking enabled across the
capture publication callback.

CTest also runs `soundfork_discord_relay --list-endpoints`; this only enumerates
active WASAPI render endpoints and does not change audio settings.

`soundfork_control --self-test` exercises the control application's registry,
ASIO enumeration, WASAPI endpoint enumeration, and read-only shared-ring
observer without showing UI or changing configuration. The capture-ring test
also verifies that a status observer does not claim the single consumer slot.

For Phase 2, `soundfork_host --proxy-backend <name>` creates the SoundFork proxy
core in-process and wraps an already registered hardware driver. This exercises
the proxy data path without installing SoundFork or requiring administrator
rights.

## Manual registration test

This requires prior approval and an elevated 64-bit PowerShell:

1. Record the existing `HKLM\SOFTWARE\ASIO` entries.
2. Run `Register-SoundFork.ps1` against the Release DLL.
3. Confirm `soundfork_host --list` shows `SoundFork`.
4. Run the query-only `--driver SoundFork` probe.
5. Run `Unregister-SoundFork.ps1`.
6. Confirm the SoundFork COM and ASIO keys are gone and the FiiO keys are
   unchanged.

## Phase 2 hardware results

On 2026-10-18, the installed FiiO ASIO Driver 6.16.0.0 and FiiO K11 R2R
successfully ran both the direct and in-process SoundFork proxy paths at
48 kHz / 64 samples with two `ASIOSTInt32LSB` output channels. Each five-second
silent proxy run delivered 3,751 callbacks; a later two-second comparison
delivered 1,500 proxy callbacks and 1,501 direct callbacks. Both paths started,
streamed, stopped, and disposed their buffers without an ASIO reset request.
A 30-second proxy soak delivered 22,501 callbacks without a reset/resync request
or process error (22,500 nominal callbacks at 750 callbacks/second).

At the active configuration, the FiiO driver reported 168 samples (3.5 ms) of
output latency. The single-buffer duration is 64 / 48,000 = 1.333 ms. These are
driver-reported and theoretical values, not a physical loopback measurement.
SoundFork adds no reported ASIO latency because it aliases the FiiO buffers.

The host supports `sine-left` and `sine-right`; their default level is -40 dBFS.
The user confirmed DJMAX stereo playback, no perceptible latency
difference from direct FiiO ASIO, no clicks/pops/dropouts, and successful game
restart.

## Phase 3 hardware results

The FiiO-backed proxy and `soundfork_capture_monitor` ran as separate processes
at 48 kHz / 64 samples. During a four-second consumer window, the monitor read
2,993 blocks / 191,552 frames with zero producer drops while the ASIO host
completed 3,751 callbacks over five seconds.

In a late-consumer test, the consumer intentionally attached two seconds after
playback began. Capture dropped 1,471 blocks while no consumer was available,
then recovered and consumed 1,495 blocks / 95,680 frames. The FiiO path still
completed 3,751 callbacks and stopped normally. This validates failure
isolation for an absent or delayed broadcast consumer.

The installed COM driver path (`soundfork_host --driver SoundFork`) was then
tested with the external monitor. Over a three-second consumer window it
delivered 2,244 blocks / 143,616 frames with zero drops, while the registered
driver completed 3,001 callbacks over four seconds.

Measured callback overhead and physical round-trip latency must be reported
separately from the theoretical 64 / 48000 = 1.333 ms buffer duration.

## Not yet implemented or claimed

- nonzero DJMAX music capture through the shared-memory monitor
- multi-hour capture soak testing
- Discord DJMAX application-window audio delivery
- microphone plus entire-screen relay audio simultaneous remote confirmation
- OBS integration
- physical loopback measurement of actual latency

## Phase 4 experimental result

On the current Windows build, `IAudioViewManagerService` successfully associated
a SoundFork relay stream with a visible test window. The relay rendered 96,960
frames through the existing `VoiceMeeterDesktop` endpoint with zero underflow
and zero capture drops while the FiiO ASIO path continued normally. This first
proved the public Windows transport and window association; Discord inclusion
was evaluated separately in the subsequent listener test.

The subsequent Discord test established the boundary: DJMAX application-window
sharing did not include the associated relay stream, while entire-monitor
sharing did include the relay audio. `--system-share` now exposes that verified
fallback without requiring a target window or Windows 11 HWND association.
The entire-screen path was subsequently verified through base VB-CABLE as well;
direct ASIO without the relay remained absent from Discord capture.

## Phase 5 control application checklist

The initial native control application requires these manual UI checks:

1. It shows the registered SoundFork DLL and currently selected hardware ASIO
   backend without requesting elevation.
2. The backend combo excludes SoundFork itself and includes registered x64 ASIO
   drivers. Applying a change requests UAC and takes effect after the host is
   restarted.
3. The endpoint combo lists active WASAPI render endpoints and remembers the
   selection in the current-user registry. It prefers base VB-CABLE, then
   VoiceMeeter Desktop, and does not automatically choose a physical speaker.
4. Starting the relay creates one persistent hidden relay process; pressing
   Stop requests a graceful shutdown rather than terminating it.
5. With an ASIO stream active, format, producer/consumer PIDs, block counters,
   and capture drops refresh without taking the SPSC consumer slot.
6. Closing the control window neither stops DAC playback nor an active relay.
7. With no supported virtual endpoint installed, **Broadcast: Not ready** is
   shown and **Get VB-CABLE** opens <https://vb-audio.com/Cable/>.
8. With VB-CABLE installed, refreshing devices marks it **Recommended**,
   selects it ahead of VoiceMeeter, and shows **Broadcast: Ready (VB-CABLE)**.

## Latest automated result

On 2026-10-19, all six CTest cases passed in both Debug and Release: proxy
core, COM server, ASIO host enumeration, shared-memory capture ring, Discord
relay endpoint listing, and the control application self-test.

## CI and release-package tests

Pull requests and pushes to `main` run the complete CTest set in both Windows
x64 Debug and Release. A tag workflow additionally verifies:

1. The tag's base SemVer matches the CMake project version.
2. The tagged commit is contained in `main`.
3. Release PE files are x64 and have no dynamic MSVC runtime dependency.
4. Recomputed SHA-256 values match the published checksum file.
5. Automated Setup installation using a temporary x64 ASIO registry backend.
6. SoundFork COM/ASIO registration and the selected `BackendCLSID`.
7. The installed control application's self-test.
8. Uninstall removes only SoundFork and preserves the fake backend.

`Test-ReleaseInstaller.ps1` temporarily changes HKLM and therefore refuses to
run unless `-AllowRegistryChanges` is explicitly passed on an isolated
administrator test machine. It is not run by default on a development PC and
aborts instead of overwriting any existing SoundFork or test registry key.
