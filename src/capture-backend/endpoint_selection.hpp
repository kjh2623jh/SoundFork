#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace soundfork::capture {

// Device IDs are opaque: never interpret them as friendly names or substrings
// when reopening a saved output. This runs only on the relay control thread.
inline bool endpoint_matches(std::wstring_view id, std::wstring_view name,
                             std::wstring_view requested, bool exact_id) {
  if (requested.empty()) return false;
  if (exact_id) return id == requested;
  const auto lower = [](std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t c) {
      return static_cast<wchar_t>(std::towlower(c));
    });
    return result;
  };
  const auto needle = lower(requested);
  return lower(id).find(needle) != std::wstring::npos ||
         lower(name).find(needle) != std::wstring::npos;
}

}  // namespace soundfork::capture
