#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include "control-app/update_preferences.hpp"

int main() {
  using namespace soundfork::control;
  const auto path = L"Software\\SoundForkUpdateTest-" + std::to_wstring(GetCurrentProcessId());
  int result = 0;
  try {
    if (!automatic_updates_enabled(HKEY_CURRENT_USER, path.c_str())) throw std::runtime_error("default disabled");
    if (save_automatic_updates(false, HKEY_CURRENT_USER, path.c_str()) != ERROR_SUCCESS ||
        automatic_updates_enabled(HKEY_CURRENT_USER, path.c_str())) throw std::runtime_error("opt-out not retained");
    if (save_automatic_updates(true, HKEY_CURRENT_USER, path.c_str()) != ERROR_SUCCESS ||
        !automatic_updates_enabled(HKEY_CURRENT_USER, path.c_str())) throw std::runtime_error("opt-in not retained");
    if (save_automatic_updates(false, nullptr, path.c_str()) == ERROR_SUCCESS)
      throw std::runtime_error("invalid registry root accepted");
    std::cout << "Automatic-update preference defaults, persistence and invalid-root handling passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
  RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  return result;
}
