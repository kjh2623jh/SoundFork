#include "audio-bridge/backend_factory.hpp"

#include <array>

#include "audio-bridge/ids.hpp"
#include "audio-bridge/windows_audio_driver.hpp"
#include "audio-bridge/windows_output_settings.hpp"

namespace soundfork {
namespace {

HRESULT read_backend_clsid(CLSID* clsid) noexcept {
  if (clsid == nullptr) {
    return E_POINTER;
  }

  HKEY key = nullptr;
  const LSTATUS open_status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE, kDriverRegistryPath, 0, KEY_QUERY_VALUE, &key);
  if (open_status != ERROR_SUCCESS) {
    return HRESULT_FROM_WIN32(open_status);
  }

  std::array<wchar_t, 64> value{};
  DWORD type = 0;
  DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
  const LSTATUS query_status = RegQueryValueExW(
      key,
      L"BackendCLSID",
      nullptr,
      &type,
      reinterpret_cast<BYTE*>(value.data()),
      &size);
  RegCloseKey(key);

  if (query_status != ERROR_SUCCESS) {
    return HRESULT_FROM_WIN32(query_status);
  }
  if (type != REG_SZ) {
    return HRESULT_FROM_WIN32(ERROR_DATATYPE_MISMATCH);
  }

  const HRESULT parse_result = CLSIDFromString(value.data(), clsid);
  if (FAILED(parse_result)) {
    return parse_result;
  }
  if (IsEqualCLSID(*clsid, kSoundForkClsid)) {
    return HRESULT_FROM_WIN32(ERROR_CIRCULAR_DEPENDENCY);
  }
  return S_OK;
}

}  // namespace

HRESULT RegistryBackendFactory::create(IASIO** driver) noexcept {
  bool windows = false;
  return create_with_mode(driver, &windows);
}
HRESULT RegistryBackendFactory::create_with_mode(IASIO** driver, bool* windows) noexcept {
  if(!windows) return E_POINTER;
  *windows = false;
  if (driver == nullptr) {
    return E_POINTER;
  }
  *driver = nullptr;

  try {
    std::wstring endpoint;
    const auto selected = read_windows_output(&endpoint);
    if(FAILED(selected)) return selected;
    if(selected == S_OK) { *driver = new WindowsAudioDriver(std::move(endpoint)); *windows = true; return S_OK; }
  } catch (...) { return E_OUTOFMEMORY; }

  CLSID backend_clsid{};
  const HRESULT config_result = read_backend_clsid(&backend_clsid);
  if (FAILED(config_result)) {
    return config_result;
  }

  return CoCreateInstance(
      backend_clsid,
      nullptr,
      CLSCTX_INPROC_SERVER,
      backend_clsid,
      reinterpret_cast<void**>(driver));
}

ClsidBackendFactory::ClsidBackendFactory(CLSID backend_clsid) noexcept
    : backend_clsid_(backend_clsid) {}

HRESULT ClsidBackendFactory::create(IASIO** driver) noexcept {
  if (driver == nullptr) {
    return E_POINTER;
  }
  *driver = nullptr;
  if (IsEqualCLSID(backend_clsid_, kSoundForkClsid)) {
    return HRESULT_FROM_WIN32(ERROR_CIRCULAR_DEPENDENCY);
  }
  return CoCreateInstance(
      backend_clsid_,
      nullptr,
      CLSCTX_INPROC_SERVER,
      backend_clsid_,
      reinterpret_cast<void**>(driver));
}

}  // namespace soundfork
