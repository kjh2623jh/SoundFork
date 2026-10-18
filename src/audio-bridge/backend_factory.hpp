#pragma once

#include <windows.h>
#include <objbase.h>

#include "iasiodrv.h"

namespace soundfork {

class IBackendFactory {
 public:
  virtual ~IBackendFactory() = default;
  virtual HRESULT create(IASIO** driver) noexcept = 0;
};

class RegistryBackendFactory final : public IBackendFactory {
 public:
  HRESULT create(IASIO** driver) noexcept override;
};

class ClsidBackendFactory final : public IBackendFactory {
 public:
  explicit ClsidBackendFactory(CLSID backend_clsid) noexcept;
  HRESULT create(IASIO** driver) noexcept override;

 private:
  CLSID backend_clsid_{};
};

}  // namespace soundfork
