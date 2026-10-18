#include <windows.h>
#include <objbase.h>

#include <cstring>
#include <iostream>

#include "audio-bridge/ids.hpp"
#include "iasiodrv.h"

namespace {

using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using CanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();

int fail(const char* message) {
  std::cerr << "SoundFork COM test failure: " << message << '\n';
  return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    return fail("expected path to soundfork_driver.dll");
  }

  HMODULE module = LoadLibraryW(argv[1]);
  if (module == nullptr) {
    return fail("LoadLibraryW failed");
  }

  auto get_class_object = reinterpret_cast<GetClassObjectFn>(
      GetProcAddress(module, "DllGetClassObject"));
  auto can_unload_now = reinterpret_cast<CanUnloadNowFn>(
      GetProcAddress(module, "DllCanUnloadNow"));
  if (get_class_object == nullptr || can_unload_now == nullptr) {
    FreeLibrary(module);
    return fail("required COM exports are missing");
  }

  CLSID wrong_clsid{};
  IClassFactory* factory = nullptr;
  if (get_class_object(
          wrong_clsid,
          IID_IClassFactory,
          reinterpret_cast<void**>(&factory)) != CLASS_E_CLASSNOTAVAILABLE) {
    FreeLibrary(module);
    return fail("unknown CLSID was accepted");
  }

  HRESULT result = get_class_object(
      soundfork::kSoundForkClsid,
      IID_IClassFactory,
      reinterpret_cast<void**>(&factory));
  if (FAILED(result) || factory == nullptr) {
    FreeLibrary(module);
    return fail("class factory creation failed");
  }

  IASIO* driver = nullptr;
  result = factory->CreateInstance(
      nullptr,
      soundfork::kSoundForkClsid,
      reinterpret_cast<void**>(&driver));
  if (FAILED(result) || driver == nullptr) {
    factory->Release();
    FreeLibrary(module);
    return fail("IASIO creation failed");
  }

  char name[32]{};
  driver->getDriverName(name);
  if (std::strcmp(name, "SoundFork") != 0) {
    driver->Release();
    factory->Release();
    FreeLibrary(module);
    return fail("unexpected driver name");
  }

  if (can_unload_now() != S_FALSE) {
    driver->Release();
    factory->Release();
    FreeLibrary(module);
    return fail("DLL reported unloadable with live COM objects");
  }

  driver->Release();
  factory->Release();
  if (can_unload_now() != S_OK) {
    FreeLibrary(module);
    return fail("DLL retained COM objects after release");
  }

  FreeLibrary(module);
  std::cout << "SoundFork COM server tests passed\n";
  return 0;
}

