# SoundFork

[한국어](README.md) | **English**

> This project was developed with AI assistance. Architecture decisions, code
> review, and real-hardware and Discord validation were performed by the project
> maintainer.

SoundFork is a low-latency ASIO proxy for Windows x64. It sends game audio
directly through a hardware ASIO driver to the DAC while copying the same audio
for Discord entire-screen sharing.

## Download

[**View the latest release**](../../releases) ·
[**Download Windows x64 Setup**](../../releases/download/v0.1.0-alpha.1/SoundFork-Setup-v0.1.0-alpha.1.exe) ·
[Download ZIP](../../releases/download/v0.1.0-alpha.1/SoundFork-v0.1.0-alpha.1-windows-x64.zip)

The current version is the unsigned `v0.1.0-alpha.1`. Windows SmartScreen may
show a warning. Verify downloads using the accompanying `SHA256SUMS.txt`. The
automatic GitHub “Source code” archives are not executable Windows packages.

## Features

- Direct exposure of hardware ASIO buffers without an extra playback buffer
- Nonblocking audio duplication that cannot wait for a slow broadcast consumer
- Selection of registered x64 ASIO drivers, including FiiO
- Automatic preference for VB-CABLE with VoiceMeeter as a fallback
- SoundFork Control for backend, stream, relay, and drop status
- Setup and Start-menu uninstallation

SoundFork is not tied to DJMAX. Other games and DAWs using standard x64 ASIO
can select it. The real-hardware validation environment is currently DJMAX
RESPECT V, FiiO K11 R2R, and 48 kHz / 64 samples.

## Requirements

- Windows 10/11 x64
- An x64 ASIO driver for the DAC or audio interface
- [VB-CABLE](https://vb-audio.com/Cable/) recommended for Discord

SoundFork neither bundles nor removes FiiO, VB-CABLE, or VoiceMeeter and does
not change Windows default playback or recording devices.

## Installation

1. Download and run `SoundFork-Setup-vX.Y.Z.exe` from a Release.
2. Select the hardware ASIO driver SoundFork should wrap. `FiiO ASIO Driver` is
   preferred automatically when present.
3. Open **SoundFork Control** after installation.
4. Select **SoundFork** as the ASIO driver in the game or DAW.

The control application is not required for playback. Closing its window does
not stop DAC output or a relay that is already running.

## Discord entire-screen sharing

```text
game -> SoundFork -> FiiO ASIO -> DAC
                 +-> relay -> VB-CABLE -> Discord entire-screen share
```

1. Install VB-CABLE and reboot if instructed by its vendor.
2. Select `CABLE Input` as the Broadcast endpoint in **SoundFork Control**.
3. Start the game and press **Start relay**.
4. Share the **entire monitor** in Discord, not the application window.
5. Keep the normal physical microphone selected in Discord.

Do not select `CABLE Output` as the Discord microphone or enable Windows
**Listen to this device**. Discord application-window sharing currently excludes
audio owned by the separate relay process and is not supported.

## Uninstallation

Use **Windows Settings → Apps → Installed apps → SoundFork → Uninstall** or
**Uninstall SoundFork** in the Start menu. Close the game and relay first.
Hardware ASIO drivers and VB-CABLE are not removed.

## Verified status

- no perceptible latency regression versus direct FiiO ASIO
- stereo output, restart behavior, and no clicks/pops/dropouts verified
- game playback continues when the broadcast consumer stops
- Discord entire-screen audio verified through VB-CABLE
- Discord application-window sharing is unsupported
- OBS and physical loopback latency remain unverified

See the [developer documentation](docs/DEVELOPMENT.en.md) for builds,
architecture, test measurements, and release procedures.

## License

SoundFork is GPL-3.0-only. See [LICENSE](LICENSE) and
[third-party notices](THIRD_PARTY_NOTICES.en.md).
