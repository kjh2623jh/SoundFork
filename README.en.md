# SoundFork

[한국어](README.md) | **English**

> This project was developed with AI assistance. Architecture decisions, code
> review, and real-hardware validation were performed by the project maintainer.

SoundFork is a low-latency ASIO bridge for Windows x64. It plays audio from a
game or DAW through the existing hardware ASIO driver and DAC while copying the
same audio to a virtual audio device for capture by Discord and OBS.

## Download

[**Download Windows x64 Setup**](../../releases/download/v0.1.0-beta.1/SoundFork-Setup-v0.1.0-beta.1.exe) ·
[Download ZIP](../../releases/download/v0.1.0-beta.1/SoundFork-v0.1.0-beta.1-windows-x64.zip) ·
[View all releases](../../releases)

The current version is the unsigned `v0.1.0-beta.1`, so Windows SmartScreen may
show a warning. For installation, use the `SoundFork-Setup` file rather than a
GitHub-generated “Source code” archive.

## Requirements

- Windows 10/11 x64
- An x64 ASIO driver for the DAC or audio interface
- A virtual audio render endpoint for Discord or OBS capture

[VB-CABLE](https://vb-audio.com/Cable/) is recommended. An existing VoiceMeeter
virtual render endpoint can also be used. SoundFork neither bundles nor removes
these external devices and does not change Windows default audio devices.

## Installation and playback

1. Run `SoundFork-Setup-v0.1.0-beta.1.exe`.
2. Select the hardware ASIO driver SoundFork should use.
3. Select **SoundFork** as the ASIO driver in the game or DAW.
4. For broadcasting or recording, select a virtual render endpoint in
   **SoundFork Control** and press **Start relay**.

Closing the control panel exits it when the relay is stopped. While the relay is
running, the panel hides in the notification area; use its tray menu to reopen
the panel or stop the relay. Change the display language from **Settings →
Language** and check for new versions from **Help → Check for updates**.

## Discord entire-screen sharing

1. Select `CABLE Input` as the broadcast endpoint in **SoundFork Control**.
2. Start the game or DAW and press **Start relay**.
3. Share the **entire monitor** in Discord instead of an application window.
4. Keep the normal physical microphone selected in Discord.

When using another virtual audio device, select its virtual render endpoint.
Discord application-window sharing does not include audio from the separate
relay and is not supported.

## OBS capture

Development builds include an optional **SoundFork Audio** source plugin. It is
not included in the existing `v0.1.0-beta.1` download.

Select **Install the OBS audio source plugin** in the new installer, restart OBS,
and add a **SoundFork Audio** source. Requires OBS Studio 32.2.2 or later and
stereo 48 kHz capture. OBS-only capture needs neither a virtual audio device nor
the Discord relay. Start the relay as usual for simultaneous Discord sharing.
Reuse the existing SoundFork source in other scenes instead of creating another.

For later installation, updates or removal, use **SoundFork Control → Settings →
OBS plugin**. The menu shows installed and included versions. Close OBS before
file operations and approve the administrator prompt. Control volume and mute in OBS.

The existing virtual-device method below is also supported. Choose one OBS
capture method to avoid capturing the game twice.

1. Select `CABLE Input` in **SoundFork Control** and start the relay.
2. Add an **Audio Output Capture** source in OBS and select `CABLE Input`.

Alternatively, select `CABLE Input` under **Settings → Audio → Desktop Audio**.
When using another virtual audio device, select the same virtual render endpoint
in both SoundFork and OBS.

VB-CABLE's `CABLE Output` is a recording endpoint, so it does not appear in the
OBS Desktop Audio or Audio Output Capture device list and is not needed for this
configuration.

## Uninstallation

Use **Windows Settings → Apps → Installed apps → SoundFork → Uninstall** or
**Uninstall SoundFork** in the Start menu. Hardware ASIO drivers and virtual
audio devices are not removed.

## More information

See the [developer documentation](docs/DEVELOPMENT.en.md) for architecture,
builds, tests, and known technical limitations. SoundFork is GPL-3.0-only; also
see [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.en.md).
