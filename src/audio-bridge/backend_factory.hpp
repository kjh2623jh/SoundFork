#pragma once

#include <windows.h>
#include <objbase.h>

#include "iasiodrv.h"

namespace soundfork {

class IBackendFactory {
 public:
  virtual ~IBackendFactory() = default;
  virtual HRESULT create(IASIO** driver) noexcept = 0;
  virtual HRESULT create_with_mode(IASIO** driver, bool* windows) noexcept {
    if(!windows) return E_POINTER;
    *windows = false; return create(driver);
  }
};

class RegistryBackendFactory final : public IBackendFactory {
 public:
  HRESULT create(IASIO** driver) noexcept override;
  HRESULT create_with_mode(IASIO** driver, bool* windows) noexcept override;
};

class ClsidBackendFactory final : public IBackendFactory {
 public:
  explicit ClsidBackendFactory(CLSID backend_clsid) noexcept;
  HRESULT create(IASIO** driver) noexcept override;

 private:
  CLSID backend_clsid_{};
};

}  // namespace soundfork
