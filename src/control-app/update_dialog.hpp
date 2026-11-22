#pragma once
#include <windows.h>
#include "control-app/update_checker.hpp"

namespace soundfork::control {
// Posted to the parent when the notice is dismissed or Setup has started.
// wParam contains the notice HWND, lParam contains UpdateNoticeFlags.
inline constexpr UINT kUpdateNoticeClosed = WM_APP + 21;
enum UpdateNoticeFlags : LPARAM { disable_update_notifications = 1, update_installer_started = 2 };
HWND create_update_notice(HWND parent, bool korean, UpdateResult release);
void translate_update_notice(HWND notice, bool korean);
}  // namespace soundfork::control
