#pragma once

#include <audioclient.h>

namespace soundfork::capture {

inline bool recoverable_audio_error(HRESULT result) noexcept {
  return result == AUDCLNT_E_DEVICE_INVALIDATED ||
         result == AUDCLNT_E_SERVICE_NOT_RUNNING ||
         result == AUDCLNT_E_RESOURCES_INVALIDATED ||
         result == AUDCLNT_E_ENDPOINT_CREATE_FAILED;
}

}  // namespace soundfork::capture
