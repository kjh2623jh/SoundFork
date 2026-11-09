#pragma once
#include "capture-backend/relay_preferences.hpp"

namespace soundfork::capture {
inline constexpr DWORD kMaximumCaptureChannels = 64;
struct ChannelPair { DWORD left = 0, right = 1; };
constexpr bool valid_channels(ChannelPair pair) noexcept {
  return pair.left < kMaximumCaptureChannels && pair.right < kMaximumCaptureChannels;
}
constexpr DWORD encode_channels(ChannelPair pair) noexcept { return pair.left | (pair.right << 16); }
constexpr ChannelPair decode_channels(DWORD value) noexcept {
  const ChannelPair pair{value & 0xffffU, value >> 16};
  return valid_channels(pair) ? pair : ChannelPair{};
}
inline ChannelPair load_channels() noexcept {
  return decode_channels(relay_preference(L"CaptureChannelPair", encode_channels({})));
}
inline DWORD save_channels(ChannelPair pair) noexcept {
  // One value: the proxy must never observe a half-updated left/right pair.
  return valid_channels(pair) ? save_relay_preference(L"CaptureChannelPair", encode_channels(pair)) : ERROR_INVALID_PARAMETER;
}
}
