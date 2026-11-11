#pragma once
#include <windows.h>

namespace soundfork::platform {
// Registry keys have a different closer from kernel handles.
class UniqueRegistryKey final {
 public:
  UniqueRegistryKey() = default;
  ~UniqueRegistryKey() { if (value) RegCloseKey(value); }
  UniqueRegistryKey(const UniqueRegistryKey&) = delete;
  UniqueRegistryKey& operator=(const UniqueRegistryKey&) = delete;
  HKEY value = nullptr;
};
}  // namespace soundfork::platform
