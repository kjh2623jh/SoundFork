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

[**Download Windows x64 Setup**](../../releases/download/v0.1.0-beta.2/SoundFork-Setup-v0.1.0-beta.2.exe) ·
[View all releases](../../releases)

The current version is the unsigned `v0.1.0-beta.2`, so Windows SmartScreen may
show a warning.

## Requirements

- Windows 10/11 x64
- An x64 game or DAW with ASIO support
- Your DAC or audio interface manufacturer's x64 ASIO driver, installed beforehand

Broadcast capture currently supports **48 kHz, stereo, 32-bit integer ASIO output**.
Real-use testing has been with DJMAX RESPECT V and the FiiO K11 R2R.

## Installation and playback

1. Run Setup and select the **hardware ASIO driver** SoundFork should use.
2. Select **SoundFork** as the ASIO driver in the game or DAW and set the sample rate to **48 kHz**.

Open the control panel from **SoundFork Control** in the Start menu. For game
playback alone, neither the control panel nor the relay needs to stay open.

## Discord entire-screen sharing

Install a virtual audio device separately; [VB-CABLE](https://vb-audio.com/Cable/) is recommended.
With VB-CABLE:

1. Select `CABLE Input` as the **Broadcast render endpoint** in the control panel.
2. Press **Start relay** and play audio in the game or DAW.
3. Share the **entire monitor** in Discord. **Application-window sharing is not supported.**

When using another virtual audio device, select its virtual playback device instead of `CABLE Input`.

Closing the control panel while the relay is running hides it in the notification
area (system tray), and broadcasting continues. To finish, choose **Stop relay and
exit** from the tray menu.

## OBS streaming and recording

### Dedicated plugin

Requires OBS Studio **32.2.2 or later, x64**.

1. Close OBS and select **Install the OBS audio source plugin** during SoundFork Setup.
   If SoundFork is already installed, use **Settings → OBS plugin → Install** in the control panel.
2. Start OBS and add a **SoundFork Audio** source.
3. Reuse that existing SoundFork source in other scenes rather than creating another.

For OBS alone, **no virtual audio device or relay is needed**.

### Virtual audio device

1. Select `CABLE Input` in the control panel and press **Start relay**.
2. Add an **Audio Output Capture** source in OBS and select the same `CABLE Input`.
   Alternatively, select it under **Settings → Audio → Desktop Audio**.

For another virtual device, select the same virtual playback device in both SoundFork and OBS.

**In OBS, use only one capture method for the same game audio to avoid duplicate sound.**

## Updates, settings and removal

Close games/DAWs and OBS and stop the relay before updating or uninstalling.

- **Updates:** Use **Help → Check for updates** in the control panel, then run the new Setup.
  If you use the OBS plugin, select its installation option to update it at the same time.
- **Language:** Choose English or Korean under **Settings → Language** in the control panel.
- **OBS plugin management:** Install, update or remove it under **Settings → OBS plugin**.
  Close OBS first; these operations require administrator approval.
- **Removal:** Uninstall SoundFork from **Windows Settings → Apps** or run
  **Uninstall SoundFork** from the Start menu. Hardware ASIO drivers and virtual audio devices are kept.

## Project information

This project was developed with AI assistance and is distributed under GPL-3.0-only.

[Program details](docs/DETAILS.en.md) · [Development guide](docs/DEVELOPMENT.en.md) · [License](LICENSE) ·
[Third-party notices](THIRD_PARTY_NOTICES.en.md)
