#include <windows.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "platform/unique_handle.hpp"

using soundfork::platform::UniqueHandle;
static_assert(!std::is_copy_constructible_v<UniqueHandle>);
static_assert(!std::is_copy_assignable_v<UniqueHandle>);
static_assert(std::is_nothrow_move_constructible_v<UniqueHandle>);
static_assert(std::is_nothrow_move_assignable_v<UniqueHandle>);
static_assert(sizeof(UniqueHandle) == sizeof(HANDLE));

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
bool closed(HANDLE handle) {
  DWORD flags = 0;
  return !GetHandleInformation(handle, &flags) && GetLastError() == ERROR_INVALID_HANDLE;
}
void test_ownership() {
  UniqueHandle empty, invalid(INVALID_HANDLE_VALUE);
  require(!empty && !invalid, "failure sentinel considered valid");
  HANDLE raw = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  require(raw != nullptr, "create event");
  {
    UniqueHandle owner(raw);
    UniqueHandle moved(std::move(owner));
    require(!owner && moved.value == raw, "move did not transfer ownership");
    UniqueHandle replacement(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    require(static_cast<bool>(replacement), "create replacement");
    const HANDLE previous = replacement.value;
    replacement = std::move(moved);
    require(!moved && closed(previous), "move assignment leaked old handle");
    replacement.reset(replacement.value);
    require(SetEvent(raw) && WaitForSingleObject(raw, 0) == WAIT_OBJECT_0,
            "same-handle reset invalidated resource");
    const HANDLE released = replacement.release();
    require(!replacement && released == raw && CloseHandle(released), "release failed");
  }
  require(closed(raw), "resource remained open");
  std::array<HANDLE, 3> events{};
  {
    std::vector<UniqueHandle> owners;
    for (auto& event : events) {
      event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      require(event != nullptr, "create vector event");
      owners.emplace_back(event);
    }
    for (HANDLE event : events)
      require(SetEvent(event) && WaitForSingleObject(event, 0) == WAIT_OBJECT_0,
              "vector reallocation closed a live resource");
  }
  for (HANDLE event : events) require(closed(event), "scope cleanup leaked resource");
}
}  // namespace

int main() {
  try { test_ownership(); std::cout << "Handle ownership, transfer and cleanup passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
