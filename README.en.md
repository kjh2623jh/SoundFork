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

## Requirements

- Windows 10/11 x64
- A 64-bit or 32-bit game or DAW with ASIO support
- Vendor ASIO output needs an ASIO driver matching the game or DAW's bitness. Windows output mode needs no separate ASIO driver.

Broadcast capture supports **supported sample rates from 32 to 384 kHz and Int16, Int24, Int32, Float32 and Float64 formats**.
MSB and valid-bit formats are also supported; see [Program details](docs/DETAILS.en.md) for the exact list and limits.
Broadcast audio uses two selected outputs as stereo. Change them under **Buffer/ch. → Channels**
(default: outputs 1/2), then restart the game/DAW. Only outputs opened by the application can be captured.
Windows output mode exposes **48 kHz stereo ASIO input**.
Real-use testing has been with DJMAX RESPECT V and the FiiO K11 R2R.

PCM formats and sample-rate handling have automated coverage. Other vendor drivers and additional formats still need real-device compatibility testing.

## Installation and playback

1. Run Setup and select the playback output.
2. Select **SoundFork** as the ASIO driver in the game or DAW. For Windows output mode, set the sample rate to **48 kHz**.

Choose `ASIO — driver name` or `Windows — speakers/headphones`
under **Playback output** and press **Apply**, then restart the application. Vendor ASIO
is the recommended low-latency path; Windows output uses shared-mode WASAPI. The Windows
default device is not changed. The **Forwarding device** is a separate broadcast selection.
Windows output already belongs to the application, so no additional internal relay is loaded.
Disable forwarding to avoid duplicate capture and use the dedicated OBS source.

Open the control panel from **SoundFork Control** in the Start menu. For game
playback alone, neither the control panel nor audio forwarding needs to stay open.

## Discord sharing

With **Windows output mode**, Discord can capture the game's audio directly when sharing its window.
Try sharing with audio forwarding disabled first.

**Vendor ASIO output** needs audio forwarding and a separately installed virtual audio device.
[VB-CABLE](https://vb-audio.com/Cable/) is recommended. With VB-CABLE:

1. Select `CABLE Input` as the **Forwarding device** in the control panel.
2. Enable **audio forwarding** and select a forwarding mode.

   - `External relay — entire-screen sharing`: press **Start forwarding**, then share the entire monitor in Discord.
   - `Internal relay — application-window sharing supported`: restart the application, then share its window in Discord.

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

For OBS alone, **no virtual audio device or audio forwarding is needed**.

### Virtual audio device

1. Select `CABLE Input` in the control panel and press **Start forwarding**.
2. Add an **Audio Output Capture** source in OBS and select the same `CABLE Input`.
   Alternatively, select it under **Settings → Audio → Desktop Audio**.

For another virtual device, select the same virtual playback device in both SoundFork and OBS.

**In OBS, use only one capture method for the same game audio to avoid duplicate sound.**

## Updates, settings and removal

Close games/DAWs and OBS and stop audio forwarding before updating or uninstalling.

- **Updates:** SoundFork checks for new stable versions when you first open the control panel. Alpha, beta and rc versions are excluded.
  Disable notifications in the notice or under **Settings → Notify me about new versions**.
  **About → Check for updates** remains available. Download and run the new Setup.
  If you use the OBS plugin, select its installation option to update it at the same time.
- **Language:** Choose English or Korean under **Settings → Language** in the control panel.
- **Automatic startup:** Device selection is saved automatically. Check **Start audio forwarding at Windows sign-in** on the **Broadcast** page
  to start with the saved virtual device at your next sign-in. This is off for new installations; updates preserve your choice.
- **Detailed status:** Open **Connection → Detailed diagnostics** to inspect and copy connection information and counters.
  For troubleshooting, use **Connection → Open log folder** to view diagnostic logs.
- **OBS plugin management:** Install, update or remove it on the **OBS** page.
  Close OBS first; these operations require administrator approval.
- **Removal:** Uninstall SoundFork from **Windows Settings → Apps** or run
  **Uninstall SoundFork** from the Start menu. Your selected ASIO driver and virtual audio devices are kept.

## Project information

This project was developed with AI assistance and is distributed under GPL-3.0-only.

[Program details](docs/DETAILS.en.md) · [Development guide](docs/DEVELOPMENT.en.md) · [License](LICENSE) ·
[Third-party notices](THIRD_PARTY_NOTICES.en.md)
