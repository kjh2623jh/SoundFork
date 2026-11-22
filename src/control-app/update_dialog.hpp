#pragma once
#include <windows.h>
#include "control-app/update_checker.hpp"

namespace soundfork::control {
struct UpdateDialogResult {
  bool disable_notifications = false;
  bool installer_started = false;
};
UpdateDialogResult show_update_dialog(HWND parent, bool korean, const UpdateResult&);
}  // namespace soundfork::control
