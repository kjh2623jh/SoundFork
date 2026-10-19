#pragma once

#include <string>

namespace soundfork::control {

enum class UpdateStatus {
  failed,
  up_to_date,
  available,
};

struct UpdateResult {
  UpdateStatus status = UpdateStatus::failed;
  std::wstring latest_tag;
};

UpdateResult check_for_updates() noexcept;
bool update_parser_self_test() noexcept;

}  // namespace soundfork::control

