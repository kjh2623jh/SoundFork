# SoundFork

[한국어](README.md) | **English**

[![CodeQL](https://github.com/kjh2623jh/SoundFork/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/kjh2623jh/SoundFork/actions/workflows/codeql.yml)
[![Release checks · ClamAV](https://github.com/kjh2623jh/SoundFork/actions/workflows/release.yml/badge.svg)](https://github.com/kjh2623jh/SoundFork/actions/workflows/release.yml)

> [!note]
> This project was developed with AI assistance

SoundFork plays a game or DAW's ASIO audio through your existing DAC with low
latency while also sending it to **Discord screen sharing** and **OBS streams or
recordings** on Windows.

## Download

[**Download Windows x64 Setup**](../../releases/download/v1.3.0/SoundFork-Setup-v1.3.0.exe) ·
[View all releases](../../releases)

Version: `v1.3.0`. One installer includes both 64-bit and 32-bit ASIO components.
Executables are not code-signed, so Windows SmartScreen may show a warning.

## Installation and playback

1. Run Setup and select the playback output.
2. Select **SoundFork** as the ASIO driver in the game or DAW.

Open the control panel from **SoundFork Control** in the Start menu. For game
playback alone, neither the control panel nor audio forwarding needs to stay open.

## Discord sharing

With **Windows output mode**, Discord can capture the game's audio directly when sharing its window.

**Vendor ASIO output** needs audio forwarding and a separately installed virtual audio device.
[VB-CABLE](https://vb-audio.com/Cable/) is recommended. With VB-CABLE:

1. Select `CABLE Input` as the **Forwarding device** in the control panel.
2. Enable **audio forwarding** and select a forwarding mode.

   - `External relay — entire-screen sharing`: press **Start forwarding**, then share the entire monitor in Discord.
   - `Internal relay — application-window sharing supported`: share the application's window or the entire monitor in Discord.

The internal relay runs inside the process using SoundFork ASIO. The forwarding buffer can use its automatic value or a frame count; the displayed duration is the amount of broadcast audio retained.

When using another virtual audio device, select its virtual playback device instead of `CABLE Input`.

Closing the control panel while audio forwarding runs hides it in the notification
area (system tray), and forwarding continues. To finish, choose **Stop forwarding and
exit** from the tray menu.

## OBS streaming and recording

### Dedicated plugin

Requires OBS Studio **32.2.2 or later, x64**.

1. Close OBS and select **Install the OBS audio source plugin** during SoundFork Setup.
   If SoundFork is already installed, use **OBS → Install** in the control panel.
2. Start OBS and add a **SoundFork Audio** source.
3. Reuse that existing SoundFork source in other scenes rather than creating another.

Choose **SoundFork Audio**, not OBS-ASIO's **ASIO Input Capture**.

With the dedicated plugin, **no virtual audio device or audio forwarding is needed**.

### Virtual audio device

1. Select `CABLE Input` in the control panel and press **Start forwarding**.
2. Add an **Audio Output Capture** source in OBS and select the same `CABLE Input`.
   Alternatively, select it under **Settings → Audio → Desktop Audio**.

For another virtual device, select the same virtual playback device in both SoundFork and OBS.

**Capturing the same audio through both the dedicated plugin and a virtual device in OBS duplicates it. Use one method.**

## Project information

This project was developed with AI assistance and is distributed under GPL-3.0-only.

[Program details](docs/DETAILS.en.md) · [Development guide](docs/DEVELOPMENT.en.md) · [License](LICENSE) ·
[Third-party notices](THIRD_PARTY_NOTICES.en.md)
