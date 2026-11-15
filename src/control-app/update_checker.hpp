#pragma once

#include <string>
#include <cstdint>
#include <string_view>

namespace soundfork::control {

enum class UpdateStatus {
  failed,
  up_to_date,
  available,
};

struct UpdateResult {
  UpdateStatus status = UpdateStatus::failed;
  std::wstring latest_tag;
  std::wstring release_notes;
  std::wstring installer_url;
  std::wstring installer_name;
  std::string sha256;
  std::uint64_t installer_size = 0;
};

UpdateResult check_for_updates() noexcept;
// Parses one GitHub release object. Missing/unverifiable assets leave the
// release notice available, but disable in-app download.
UpdateResult parse_update_release(std::string_view json) noexcept;
bool downloadable_update(const UpdateResult& release) noexcept;
bool update_parser_self_test() noexcept;

}  // namespace soundfork::control

