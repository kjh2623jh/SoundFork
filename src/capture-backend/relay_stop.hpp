#pragma once
#include <windows.h>

namespace soundfork::capture {
// Worker-only cancellation. Stop handles have precedence over an audio/timer
// wake, including when cancellation was requested before worker initialization.
class RelayStop final {
 public:
  RelayStop(HANDLE shared, HANDLE owned) noexcept : shared_(shared), owned_(owned) {}
  DWORD wait(DWORD timeout) const noexcept {
    const HANDLE handles[]{shared_, owned_};
    const DWORD result = WaitForMultipleObjects(owned_ ? 2 : 1, handles, FALSE, timeout);
    return result == WAIT_OBJECT_0 || (owned_ && result == WAIT_OBJECT_0 + 1)
        ? WAIT_OBJECT_0 : result;
  }
  DWORD wait_with(HANDLE event, DWORD timeout) const noexcept {
    const HANDLE handles[]{shared_, owned_ ? owned_ : event, event};
    const DWORD count = owned_ ? 3 : 2;
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (result < WAIT_OBJECT_0 + count)
      return result == WAIT_OBJECT_0 + count - 1 ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0;
    return result;
  }
 private:
  HANDLE shared_;
  HANDLE owned_;
};
}  // namespace soundfork::capture
