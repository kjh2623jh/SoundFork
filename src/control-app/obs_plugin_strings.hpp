#pragma once

namespace soundfork::control {
struct ObsPluginStrings {
  const wchar_t* menu;
  const wchar_t* installed_version;
  const wchar_t* bundled_version;
  const wchar_t* absent;
  const wchar_t* incomplete;
  const wchar_t* not_bundled;
  const wchar_t* unknown;
  const wchar_t* install;
  const wchar_t* update;
  const wchar_t* remove;
  const wchar_t* working;
  const wchar_t* information;
  const wchar_t* usage;
  const wchar_t* close_obs;
  const wchar_t* installed;
  const wchar_t* removed;
  const wchar_t* cancelled;
  const wchar_t* missing;
  const wchar_t* unsafe;
  const wchar_t* failed;
  const wchar_t* rollback;
  const wchar_t* busy;
  const wchar_t* remove_question;
};

inline const ObsPluginStrings& obs_plugin_strings(bool korean) {
  static constexpr ObsPluginStrings english{
      L"OBS plugin", L"Installed: ", L"Included: ", L"Not installed",
      L"Incomplete installation", L"Not included in this build", L"Unknown version",
      L"Install...", L"Update / reinstall...", L"Remove...", L"Working...",
      L"Plugin information / usage",
      L"Requires OBS Studio 32.2.2 or later (x64).\n\nAfter installation, restart OBS and add a SoundFork Audio source. "
      L"Reuse the existing source in other scenes. Control volume and mute in OBS.\n\n"
      L"OBS-only capture needs no virtual device or audio forwarding. For Discord, use audio forwarding from the same SoundFork build. "
      L"Avoid also capturing CABLE Input in OBS to prevent duplicate audio.\n\nInstall location:\n",
      L"Close OBS before installing, updating or removing the plugin.",
      L"OBS plugin installed. Start OBS and add a SoundFork Audio source.",
      L"OBS plugin removed. You can remove the SoundFork Audio source from your OBS scenes.",
      L"Administrator approval was cancelled. No plugin changes were requested.",
      L"The complete OBS plugin payload was not found. Install a SoundFork build that includes the OBS plugin.",
      L"The plugin path contains a link or unexpected file. No further changes were made. Check the plugin folder.",
      L"The plugin file operation failed. Close OBS and check folder permissions. If a capture process still uses the files, stop audio forwarding and retry.",
      L"The operation and rollback failed. Recovery files were retained in a soundfork-obs.transaction folder next to the plugin. Check these before retrying.",
      L"Another OBS plugin installation or removal is in progress. Try again after it finishes.",
      L"Remove the SoundFork OBS plugin? Your OBS scenes and other plugins will be kept."};
  static constexpr ObsPluginStrings ko{
      L"OBS 플러그인", L"설치된 버전: ", L"포함된 버전: ", L"설치되지 않음",
      L"불완전한 설치", L"이 빌드에 포함되지 않음", L"버전 확인 불가",
      L"설치...", L"업데이트 / 다시 설치...", L"제거...", L"작업 중...",
      L"플러그인 정보 / 사용법",
      L"OBS Studio 32.2.2 이상(x64)이 필요합니다.\n\n설치 후 OBS를 다시 실행하고 SoundFork 오디오 소스를 추가하세요. "
      L"다른 장면에는 기존 소스를 추가합니다. 볼륨과 음소거는 OBS에서 조절합니다.\n\n"
      L"OBS만 사용한다면 가상 장치나 오디오 전달이 필요하지 않습니다. Discord 동시 공유에는 같은 SoundFork 빌드의 오디오 전달을 사용하세요. "
      L"소리 중복을 피하려면 OBS의 CABLE Input 캡처를 함께 사용하지 마세요.\n\n설치 위치:\n",
      L"플러그인을 설치·업데이트·제거하려면 먼저 OBS를 종료하세요.",
      L"OBS 플러그인을 설치했습니다. OBS를 실행하고 SoundFork 오디오 소스를 추가하세요.",
      L"OBS 플러그인을 제거했습니다. OBS 장면에 남아 있는 SoundFork 오디오 소스는 직접 삭제할 수 있습니다.",
      L"관리자 권한 승인이 취소되어 플러그인 변경을 요청하지 않았습니다.",
      L"OBS 플러그인 원본 파일을 찾을 수 없습니다. 플러그인이 포함된 SoundFork 빌드를 설치하세요.",
      L"플러그인 경로에 링크 또는 예상하지 못한 파일이 있습니다. 추가 변경을 중단했습니다. 플러그인 폴더를 확인하세요.",
      L"플러그인 파일 작업에 실패했습니다. OBS 종료 여부와 폴더 권한을 확인하세요. 캡처 프로세스가 파일을 사용 중이면 오디오 전달을 중지한 후 다시 시도하세요.",
      L"파일 작업과 복구에 실패했습니다. 플러그인 옆의 soundfork-obs.transaction 폴더에 복구 파일을 보존했습니다. 다시 시도하기 전에 확인하세요.",
      L"다른 OBS 플러그인 설치 또는 제거가 진행 중입니다. 완료 후 다시 시도하세요.",
      L"SoundFork OBS 플러그인을 제거하시겠습니까? OBS 장면 설정과 다른 플러그인은 유지됩니다."};
  return korean ? ko : english;
}
}  // namespace soundfork::control
