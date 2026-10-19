# Third-party notices

[한국어](THIRD_PARTY_NOTICES.md) | **English**

## Steinberg ASIO SDK 2.3.4

SoundFork includes the unmodified ASIO ABI headers `asiosys.h`, `asio.h`, and
`iasiodrv.h` from the Steinberg ASIO SDK 2.3.4 open-source release. SoundFork
uses the GPL Version 3 option offered by the SDK's dual license.

- Upstream mirror: <https://github.com/audiosdk/asio>
- Pinned upstream commit: `496a0765b8bb9c26f764f22f9a9712a937177db2`
- Steinberg developer portal: <https://www.steinberg.net/developers/>
- SDK license: `third_party/asiosdk/LICENSE.txt`

SHA-256 checksums:

| File | SHA-256 |
| --- | --- |
| `common/asiosys.h` | `F53B4C101F84806A9320AEBB24C5B3F7A81DBE7762E558915CA2B2F85DB43BF6` |
| `common/asio.h` | `0159F2F807C49C3B165AAE87DD592DE888AFE9C2E34E1F57E089EC8F1C1A825D` |
| `common/iasiodrv.h` | `C22183608757A6CB483B412829A27DDE37978289D4932E3BA930EE6C20C35290` |
| `LICENSE.txt` | `E664C38C9A99BE3AC32D7E49D56B6E5E0F807943F4229020D2303AD60BD31306` |

ASIO is a trademark of Steinberg Media Technologies GmbH. SoundFork does not
use the ASIO logo, and ASIO is not part of the SoundFork product name.

## Optional OBS audio source

The optional plugin uses official OBS Studio 32.2.2 headers (GPL-2.0-or-later,
with notices in individual files). Source archives and SDK runtimes are downloaded
only into ignored build directories. SoundFork distributes its own GPL-3.0-only
plugin and capture hub, not OBS runtime DLLs.

- Upstream source: <https://github.com/obsproject/obs-studio/tree/32.2.2>
- License: <https://github.com/obsproject/obs-studio/blob/32.2.2/COPYING>
- Source archive SHA-256: `ec81fb66b03e75ddb3076b576f62679c39262e0e9960cef3e17a40dc5d68e6b4`
- Windows x64 archive SHA-256: `4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1`

## Optional VB-Audio software

SoundFork can detect and use an independently installed VB-CABLE or VoiceMeeter
render endpoint for Discord entire-screen audio. These products and their
drivers are not included in this repository or SoundFork installers. They are
distributed and licensed separately by VB-Audio Software:

- VB-CABLE: <https://vb-audio.com/Cable/>
- VB-Audio licensing: <https://vb-audio.com/Services/licensing.htm>

## Inno Setup build tool

The Windows `Setup.exe` is produced with Inno Setup 6. Inno Setup itself is not
included in the SoundFork source or release ZIP; only its generated installer is
published in a GitHub Release. Inno Setup does not request license purchases
from non-commercial users. Commercial users, as defined by its official terms,
are asked to purchase a commercial license when running the compiler locally or
in CI. Anyone building or distributing SoundFork commercially should review the
then-current terms.

- Inno Setup downloads: <https://jrsoftware.org/isdl.php>
- Commercial licensing: <https://jrsoftware.org/isorder.php>
