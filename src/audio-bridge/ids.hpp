#pragma once

#include <windows.h>

namespace soundfork {

// {A04A0128-E43C-4D08-B701-87EC1A847A89}
inline constexpr CLSID kSoundForkClsid{
    0xa04a0128,
    0xe43c,
    0x4d08,
    {0xb7, 0x01, 0x87, 0xec, 0x1a, 0x84, 0x7a, 0x89}};

inline constexpr wchar_t kDriverRegistryPath[] = L"SOFTWARE\\ASIO\\SoundFork";
inline constexpr wchar_t kDriverName[] = L"SoundFork";
inline constexpr char kDriverNameNarrow[] = "SoundFork";

}  // namespace soundfork

