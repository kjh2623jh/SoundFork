#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <new>
#include <string>

#include "audio-bridge/backend_factory.hpp"
#include "audio-bridge/ids.hpp"
#include "audio-bridge/proxy_driver.hpp"

namespace soundfork {
namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_factory_instances{0};
std::atomic<long> g_server_locks{0};
RegistryBackendFactory g_backend_factory;

class ClassFactory final : public IClassFactory {
 public:
  ClassFactory() noexcept {
    g_factory_instances.fetch_add(1, std::memory_order_relaxed);
  }

  ~ClassFactory() {
    g_factory_instances.fetch_sub(1, std::memory_order_relaxed);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *object = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return reference_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining =
        reference_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE CreateInstance(
      IUnknown* outer, REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (outer != nullptr) {
      return CLASS_E_NOAGGREGATION;
    }

    auto* driver = new (std::nothrow) ProxyDriver(g_backend_factory);
    if (driver == nullptr) {
      return E_OUTOFMEMORY;
    }
    const HRESULT result = driver->QueryInterface(riid, object);
    driver->Release();
    return result;
  }

  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    if (lock) {
      g_server_locks.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_server_locks.fetch_sub(1, std::memory_order_relaxed);
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> reference_count_{1};
};

HRESULT set_string_value(HKEY key, const wchar_t* name, const std::wstring& value) {
  const DWORD byte_count =
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const LSTATUS status = RegSetValueExW(
      key,
      name,
      0,
      REG_SZ,
      reinterpret_cast<const BYTE*>(value.c_str()),
      byte_count);
  return HRESULT_FROM_WIN32(status);
}

HRESULT create_key(HKEY root, const std::wstring& path, HKEY* key) {
  const LSTATUS status = RegCreateKeyExW(
      root,
      path.c_str(),
      0,
      nullptr,
      REG_OPTION_NON_VOLATILE,
      KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
      nullptr,
      key,
      nullptr);
  return HRESULT_FROM_WIN32(status);
}

std::wstring clsid_string() {
  wchar_t text[64]{};
  if (StringFromGUID2(
          kSoundForkClsid, text, static_cast<int>(std::size(text))) == 0) {
    return {};
  }
  return text;
}

HRESULT register_server() {
  wchar_t module_path[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(g_module, module_path, MAX_PATH);
  if (length == 0 || length == MAX_PATH) {
    return HRESULT_FROM_WIN32(GetLastError());
  }

  const std::wstring clsid = clsid_string();
  if (clsid.empty()) {
    return E_FAIL;
  }
  const std::wstring class_path = L"SOFTWARE\\Classes\\CLSID\\" + clsid;
  HKEY class_key = nullptr;
  HRESULT result = create_key(HKEY_LOCAL_MACHINE, class_path, &class_key);
  if (FAILED(result)) {
    return result;
  }
  result = set_string_value(class_key, nullptr, L"SoundFork");
  RegCloseKey(class_key);
  if (FAILED(result)) {
    return result;
  }

  HKEY server_key = nullptr;
  result = create_key(
      HKEY_LOCAL_MACHINE, class_path + L"\\InprocServer32", &server_key);
  if (FAILED(result)) {
    return result;
  }
  result = set_string_value(server_key, nullptr, module_path);
  if (SUCCEEDED(result)) {
    result = set_string_value(server_key, L"ThreadingModel", L"Apartment");
  }
  RegCloseKey(server_key);
  if (FAILED(result)) {
    return result;
  }

  HKEY asio_key = nullptr;
  result = create_key(HKEY_LOCAL_MACHINE, kDriverRegistryPath, &asio_key);
  if (FAILED(result)) {
    return result;
  }
  result = set_string_value(asio_key, L"CLSID", clsid);
  if (SUCCEEDED(result)) {
    result = set_string_value(asio_key, L"Description", L"SoundFork");
  }
  RegCloseKey(asio_key);
  return result;
}

HRESULT unregister_server() {
  const std::wstring clsid = clsid_string();
  if (clsid.empty()) {
    return E_FAIL;
  }
  const std::wstring class_path = L"SOFTWARE\\Classes\\CLSID\\" + clsid;

  const LSTATUS asio_status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kDriverRegistryPath);
  const LSTATUS class_status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, class_path.c_str());

  const bool asio_ok = asio_status == ERROR_SUCCESS || asio_status == ERROR_FILE_NOT_FOUND;
  const bool class_ok =
      class_status == ERROR_SUCCESS || class_status == ERROR_FILE_NOT_FOUND;
  if (!asio_ok) {
    return HRESULT_FROM_WIN32(asio_status);
  }
  if (!class_ok) {
    return HRESULT_FROM_WIN32(class_status);
  }
  return S_OK;
}

}  // namespace
}  // namespace soundfork

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    soundfork::g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

_Check_return_
STDAPI DllGetClassObject(
    _In_ REFCLSID clsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* object) {
  if (!IsEqualCLSID(clsid, soundfork::kSoundForkClsid)) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  if (object == nullptr) {
    return E_POINTER;
  }

  auto* factory = new (std::nothrow) soundfork::ClassFactory();
  if (factory == nullptr) {
    *object = nullptr;
    return E_OUTOFMEMORY;
  }
  const HRESULT result = factory->QueryInterface(riid, object);
  factory->Release();
  return result;
}

__control_entrypoint(DllExport)
STDAPI DllCanUnloadNow(void) {
  return soundfork::ProxyDriver::live_instances() == 0 &&
                 soundfork::g_factory_instances.load(std::memory_order_relaxed) == 0 &&
                 soundfork::g_server_locks.load(std::memory_order_relaxed) == 0
             ? S_OK
             : S_FALSE;
}

STDAPI DllRegisterServer() {
  return soundfork::register_server();
}

STDAPI DllUnregisterServer() {
  return soundfork::unregister_server();
}
