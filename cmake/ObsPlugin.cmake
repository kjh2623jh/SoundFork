# Consume official OBS headers, not a hand-written ABI. The preparation script
# pins and checks upstream archives; no OBS runtime is redistributed.
if(NOT EXISTS "${SOUNDFORK_OBS_SDK_ROOT}/lib/obs.lib")
  message(FATAL_ERROR "Run scripts/Prepare-ObsSdk.ps1, then set SOUNDFORK_OBS_SDK_ROOT")
endif()
set(OBS_RELEASE_CANDIDATE 0)
set(OBS_BETA 0)
configure_file(
  "${SOUNDFORK_OBS_SDK_ROOT}/obs-studio-32.2.2-sources/libobs/obsconfig.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h"
)
add_library(soundfork_obs_api INTERFACE)
target_include_directories(soundfork_obs_api SYSTEM INTERFACE
  "${SOUNDFORK_OBS_SDK_ROOT}/obs-studio-32.2.2-sources/libobs"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(soundfork_obs_api INTERFACE "${SOUNDFORK_OBS_SDK_ROOT}/lib/obs.lib")

configure_file(src/obs-plugin/soundfork_obs_version.rc.in generated/soundfork_obs_version.rc @ONLY)
add_library(soundfork_obs MODULE
  src/obs-plugin/soundfork_obs.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/soundfork_obs_version.rc"
  src/capture-backend/hub_client.cpp
  src/capture-backend/relay_clock.cpp
  src/capture-backend/shared_ring.cpp
)
target_include_directories(soundfork_obs PRIVATE src)
target_link_libraries(soundfork_obs PRIVATE soundfork_obs_api soundfork_warnings avrt)
set_target_properties(soundfork_obs PROPERTIES PREFIX "" OUTPUT_NAME "soundfork-obs")
add_dependencies(soundfork_obs soundfork_capture_hub)

# Keep a private payload beside Control for later install/update, even when the
# user did not select the optional OBS installer task. Also supports dev builds.
set(_obs_bundle "$<TARGET_FILE_DIR:soundfork_control>/obs-plugin/soundfork-obs")
add_custom_target(soundfork_obs_bundle ALL
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_obs_bundle}/bin/64bit"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:soundfork_obs>" "${_obs_bundle}/bin/64bit/soundfork-obs.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:soundfork_capture_hub>" "${_obs_bundle}/bin/64bit/soundfork_capture_hub.exe"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/src/obs-plugin/data" "${_obs_bundle}/data"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.en.md" "${_obs_bundle}"
  DEPENDS soundfork_obs soundfork_capture_hub
  VERBATIM)

install(TARGETS soundfork_obs LIBRARY DESTINATION obs-plugin/soundfork-obs/bin/64bit COMPONENT ObsPlugin)
install(PROGRAMS "$<TARGET_FILE:soundfork_capture_hub>"
  DESTINATION obs-plugin/soundfork-obs/bin/64bit COMPONENT ObsPlugin)
install(DIRECTORY src/obs-plugin/data/ DESTINATION obs-plugin/soundfork-obs/data COMPONENT ObsPlugin)
install(FILES LICENSE THIRD_PARTY_NOTICES.md THIRD_PARTY_NOTICES.en.md
  DESTINATION obs-plugin/soundfork-obs COMPONENT ObsPlugin)

if(BUILD_TESTING)
  add_executable(soundfork_obs_timeline_tests tests/obs_timeline_tests.cpp
    src/capture-backend/relay_clock.cpp src/capture-backend/shared_ring.cpp)
  target_include_directories(soundfork_obs_timeline_tests PRIVATE src)
  target_link_libraries(soundfork_obs_timeline_tests PRIVATE soundfork_warnings)
  add_test(NAME soundfork_obs_timeline_tests COMMAND soundfork_obs_timeline_tests)
  set_tests_properties(soundfork_obs_timeline_tests PROPERTIES TIMEOUT 30 LABELS "audio;unit;obs")
  add_executable(soundfork_obs_tests tests/obs_plugin_tests.cpp
    src/capture-backend/shared_ring.cpp src/capture-backend/hub_client.cpp)
  target_include_directories(soundfork_obs_tests PRIVATE src)
  target_link_libraries(soundfork_obs_tests PRIVATE soundfork_obs_api soundfork_warnings avrt)
  add_dependencies(soundfork_obs_tests soundfork_obs)
  add_test(NAME soundfork_obs_plugin_tests COMMAND soundfork_obs_tests
    "$<TARGET_FILE:soundfork_obs>" "${CMAKE_CURRENT_SOURCE_DIR}/src/obs-plugin/data")
  add_test(NAME soundfork_obs_recovery_tests COMMAND soundfork_obs_tests
    "$<TARGET_FILE:soundfork_obs>" "${CMAKE_CURRENT_SOURCE_DIR}/src/obs-plugin/data" --recovery)
  foreach(_format IN ITEMS int16 int24 float32)
    add_test(NAME soundfork_obs_${_format}_tests COMMAND soundfork_obs_tests
      "$<TARGET_FILE:soundfork_obs>" "${CMAKE_CURRENT_SOURCE_DIR}/src/obs-plugin/data" --${_format})
  endforeach()
  add_test(NAME soundfork_obs_float_recovery_tests COMMAND soundfork_obs_tests
    "$<TARGET_FILE:soundfork_obs>" "${CMAKE_CURRENT_SOURCE_DIR}/src/obs-plugin/data" --float-recovery)
  set_tests_properties(soundfork_obs_plugin_tests soundfork_obs_recovery_tests
    soundfork_obs_int16_tests soundfork_obs_int24_tests soundfork_obs_float32_tests soundfork_obs_float_recovery_tests PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SOUNDFORK_OBS_SDK_ROOT}/runtime"
    TIMEOUT 30)
  set_tests_properties(soundfork_obs_plugin_tests soundfork_obs_recovery_tests soundfork_obs_int16_tests
    soundfork_obs_int24_tests soundfork_obs_float32_tests soundfork_obs_float_recovery_tests
    PROPERTIES LABELS "audio;integration;obs")
endif()
