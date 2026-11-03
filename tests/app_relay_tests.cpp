#include <windows.h>

#include <iostream>

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) return 2;
  HMODULE module = LoadLibraryExW(
      argv[1], nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (module == nullptr) return 3;
  using Start = DWORD(WINAPI*)(const wchar_t*);
  using Stop = DWORD(WINAPI*)();
  const auto start = reinterpret_cast<Start>(
      GetProcAddress(module, "SoundForkAppRelayStart"));
  const auto stop = reinterpret_cast<Stop>(
      GetProcAddress(module, "SoundForkAppRelayStop"));
  const bool valid = start != nullptr && stop != nullptr &&
      start(nullptr) == ERROR_INVALID_PARAMETER &&
      start(L"") == ERROR_INVALID_PARAMETER &&
      stop() == ERROR_SUCCESS;
  FreeLibrary(module);
  if (!valid) return 4;
  std::cout << "Application relay exports and disabled lifecycle passed\n";
  return 0;
}
