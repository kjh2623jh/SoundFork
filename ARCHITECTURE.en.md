# SoundFork architecture

[한국어](ARCHITECTURE.md) | **English**

## Playback data path

```text
64-bit ASIO host (DJMAX, another game/DAW, or soundfork_host)
        |
        | IASIO COM calls and callbacks
        v
soundfork_driver.dll
        |
        | delegates control calls
        | aliases downstream buffer pointers (zero-copy playback)
        v
Configured hardware ASIO driver
        |
        v
DAC
```

`createBuffers` copies only the small array of `ASIOBufferInfo` descriptors.
The downstream driver's buffer pointers are written back into the host's
descriptors. During `bufferSwitch` the game therefore writes directly into the
buffer owned by the physical driver.

SoundFork currently permits one prepared stream per host process. ASIO callback
function pointers do not carry a context value, so the active instance is held
in one atomic pointer. A second active `createBuffers` returns
`ASE_InvalidMode`.

## Real-time contract

The playback forwarding path contains:

- one atomic pointer load;
- one null check;
- one direct host callback invocation.

It contains no allocation, file or registry access, logging, locks, waits,
network access, sample conversion, or additional playback buffer. When capture
has space, the callback also performs one bounded 64-frame stereo interleave
copy; when capture is full, it performs only counter checks and increments the
drop counter. Automated tests replace global allocation operators and verify
zero allocations across 10,000 callback iterations, including capture
publication.

## Phase 3 capture path

For the initial 48 kHz stereo `ASIOSTInt32LSB` format, SoundFork copies each
completed host output buffer into a named shared-memory SPSC ring:

```text
DJMAX writes FiiO-owned double buffer
        |
        +--> FiiO outputReady / DAC (unchanged zero-copy playback)
        |
        +--> interleave L/R into Local\SoundFork.Audio.v1
                                  |
                                  v
                     separate capture consumer process
```

The mapping contains 64 blocks of up to 8,192 stereo frames. The producer and
consumer publish monotonic sequence counters with Windows interlocked
operations. Neither side takes a mutex in the callback. When the consumer is
late or absent and the ring is full, the producer increments `dropped_blocks`
and immediately returns; it never overwrites unread data or delays for space.

If the ASIO host calls `outputReady()` from its callback, the proxy publishes
the capture copy first and then forwards `outputReady()` to FiiO. If the host
does not call it, the copy occurs immediately after the host callback returns.
All mapping creation, format discovery, and ownership arbitration happen in
`createBuffers`, outside the real-time callback.

`soundfork_capture_monitor` is a diagnostic consumer, while
`soundfork_discord_relay` is the current broadcast consumer. The ring permits
one active consumer at a time. `soundfork_control` observes the header through
a read-only view and never claims that consumer slot.

The real FiiO ASIO driver has been exercised through this path at 48 kHz with
64-sample buffers. Direct and proxied runs reported the same 168-sample output
latency; this is not a physical round-trip latency measurement.

## Lifecycle and errors

The proxy enforces `created -> initialized -> prepared -> running`. Cleanup
reverses the order. Backend errors are returned unchanged where ASIO provides an
error result; activation and configuration failures are available through
`getErrorMessage`.

Backend selection is stored in
`HKLM\SOFTWARE\ASIO\SoundFork\BackendCLSID`. The registration script resolves
the selected ASIO driver's existing registry entry. SoundFork never edits or
replaces the backend driver's COM registration.

## Discord and Windows audio boundary

ASIO bypasses the Windows shared audio engine, while normal WASAPI loopback
captures streams rendered through a Windows endpoint. SoundFork therefore does
not assume that direct FiiO ASIO playback will appear in Discord capture.
Application-window share, full-screen share, and an explicit capture endpoint
must be tested separately; the Phase 4 relay below provides that test path.

## Phase 4 experimental relay

`soundfork_discord_relay` consumes the Phase 3 SPSC stream, renders 48 kHz
stereo `Int32LSB` through a selected WASAPI shared-mode endpoint, and associates
that audio stream with the DJMAX `HWND` through `IAudioViewManagerService`.
Windows performs endpoint sample-rate conversion when required. The relay is a
separate process and cannot affect ASIO scheduling; underflow produces silence
only in the broadcast stream.

The association API requires Windows 11 build 22621 or later. Discord did not
include the associated stream in application-window sharing during the actual
test. Entire-screen sharing did include the relay audio, so `--system-share`
skips the ineffective HWND association and is the current verified fallback.
The relay does not use Discord injection or private APIs.

A WASAPI stream must target a real or virtual render endpoint. Sending the
duplicate to an audible endpoint causes local echo. SoundFork now prefers an
independently installed base VB-CABLE endpoint and supports VoiceMeeter as a
fallback; hardware monitoring must remain disabled. A standalone endpoint would
require a signed WaveRT/virtual audio driver because Windows does not expose a
user-mode API that creates a persistent render endpoint.

## Phase 5 control application

`soundfork_control` is an optional normal-user Win32 application. It enumerates
registered x64 ASIO drivers and active WASAPI render endpoints, reports the
SoundFork COM registration and live shared-ring counters, and starts or stops
the entire-screen relay. Selecting a new hardware backend launches only a small
elevated command in the same executable to update `BackendCLSID`; the UI and
relay stay unelevated. The ASIO driver and DAC playback do not depend on the
control application being open.

The endpoint selector preserves a valid saved virtual endpoint. Otherwise it
prefers base VB-CABLE, then VoiceMeeter Desktop, then another VoiceMeeter
endpoint. It never automatically chooses a physical device or changes the
Windows default audio device.

## Installation and release boundary

The Inno Setup installer asks for a registered x64 ASIO backend and then runs
`soundfork_control --register-driver --backend-clsid <GUID>` with its existing
administrator token. This helper can load only `soundfork_driver.dll` adjacent
to its own executable and verifies that the selected CLSID already belongs to a
registered ASIO driver. If saving the backend fails after DLL registration, it
rolls back the entire SoundFork registration. `--unregister-driver` removes
only SoundFork and leaves hardware and virtual audio drivers untouched.

Release binaries use the static MSVC runtime. The tag pipeline verifies the tag
against the CMake version and publishes the unsigned Setup.exe and ZIP only
after builds, CTest, PE dependency checks, and installer/uninstaller smoke tests
pass. Real ASIO hardware and remote Discord listening remain manual release
checks because hosted CI cannot reproduce them.
