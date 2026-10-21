cmake_minimum_required(VERSION 3.25)

project(SoundFork VERSION 1.0.0 LANGUAGES CXX)

set(
  SOUNDFORK_VERSION
  "${PROJECT_VERSION}-dev"
  CACHE STRING
  "User-visible SoundFork version"
)
if(NOT SOUNDFORK_VERSION MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+(-(alpha|beta|rc)\\.[0-9]+|-dev)?$")
  message(FATAL_ERROR "Invalid SOUNDFORK_VERSION '${SOUNDFORK_VERSION}'")
endif()

set(SOUNDFORK_VERSION_REVISION 0)
if(SOUNDFORK_VERSION MATCHES "-(alpha|beta|rc)\\.([0-9]+)$")
  set(SOUNDFORK_VERSION_REVISION "${CMAKE_MATCH_2}")
endif()
configure_file(
  src/version.hpp.in
  generated/soundfork_version.hpp
  @ONLY
)
configure_file(
  src/control-app/soundfork_version.rc.in
  generated/soundfork_version.rc
  @ONLY
)

if(NOT WIN32)
  message(FATAL_ERROR "SoundFork currently supports Windows only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "SoundFork Phase 1 supports x64 builds only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release archives are intended to run without a separately installed MSVC
# redistributable. No CRT-owned object crosses the ASIO COM boundary.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
option(SOUNDFORK_ENABLE_STATIC_ANALYSIS "Enable MSVC code analysis" OFF)
option(SOUNDFORK_BUILD_OBS_PLUGIN "Build the optional OBS audio source" OFF)
set(SOUNDFORK_OBS_SDK_ROOT "" CACHE PATH "Prepared OBS headers and import library")

add_library(soundfork_warnings INTERFACE)
target_compile_definitions(soundfork_warnings INTERFACE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)
if(MSVC)
  target_compile_options(soundfork_warnings INTERFACE
    /W4 /permissive- /utf-8 /EHsc /sdl /guard:cf
  )
  target_link_options(soundfork_warnings INTERFACE /guard:cf)
  if(SOUNDFORK_ENABLE_STATIC_ANALYSIS)
    target_compile_options(soundfork_warnings INTERFACE /analyze /WX)
  endif()
endif()

set(SOUNDFORK_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk/common")

add_library(soundfork_bridge STATIC
  src/audio-bridge/backend_factory.cpp
  src/audio-bridge/proxy_driver.cpp
  src/capture-backend/shared_ring.cpp
)
target_include_directories(soundfork_bridge
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${SOUNDFORK_SDK_DIR}"
)
target_link_libraries(soundfork_bridge PUBLIC soundfork_warnings ole32 advapi32)

add_library(soundfork_driver SHARED
  src/asio-proxy/com_server.cpp
  src/asio-proxy/soundfork.def
)
target_link_libraries(soundfork_driver PRIVATE soundfork_bridge soundfork_warnings)
set_target_properties(soundfork_driver PROPERTIES
  OUTPUT_NAME "soundfork_driver"
  PREFIX ""
)

add_library(soundfork_fake_backend STATIC
  tests/fake-asio-driver/fake_asio_driver.cpp
)
target_include_directories(soundfork_fake_backend PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake-asio-driver"
  "${SOUNDFORK_SDK_DIR}"
)
target_link_libraries(soundfork_fake_backend PUBLIC soundfork_warnings ole32)

add_executable(soundfork_tests
  tests/proxy_tests.cpp
)
target_link_libraries(soundfork_tests PRIVATE
  soundfork_bridge
  soundfork_fake_backend
  soundfork_warnings
)

add_executable(soundfork_com_tests
  tests/com_server_tests.cpp
)
target_include_directories(soundfork_com_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${SOUNDFORK_SDK_DIR}"
)
target_link_libraries(soundfork_com_tests PRIVATE soundfork_warnings ole32)
add_dependencies(soundfork_com_tests soundfork_driver)

add_executable(soundfork_host
  tests/asio-test-host/main.cpp
)

add_executable(soundfork_capture_monitor
  src/capture-backend/monitor_main.cpp
  src/capture-backend/shared_ring.cpp
)
target_include_directories(soundfork_capture_monitor PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(soundfork_capture_monitor PRIVATE soundfork_warnings)

add_executable(soundfork_capture_tests
  tests/capture_ring_tests.cpp
  src/capture-backend/shared_ring.cpp
)
target_include_directories(soundfork_capture_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(soundfork_capture_tests PRIVATE soundfork_warnings)

add_executable(soundfork_discord_relay
  src/capture-backend/discord_relay.cpp
  src/capture-backend/relay_clock.cpp
  src/capture-backend/shared_ring.cpp
  src/capture-backend/hub_client.cpp
)
target_include_directories(soundfork_discord_relay PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(soundfork_discord_relay PRIVATE
  soundfork_warnings
  ole32
  uuid
)

add_executable(soundfork_capture_hub
  src/capture-backend/capture_hub.cpp
  src/capture-backend/hub_client.cpp
  src/capture-backend/shared_ring.cpp
)
target_include_directories(soundfork_capture_hub PRIVATE src)
target_link_libraries(soundfork_capture_hub PRIVATE soundfork_warnings)
add_dependencies(soundfork_discord_relay soundfork_capture_hub)

add_executable(soundfork_hub_tests
  tests/capture_hub_tests.cpp
  src/capture-backend/hub_client.cpp
  src/capture-backend/shared_ring.cpp
)
target_include_directories(soundfork_hub_tests PRIVATE src)
target_link_libraries(soundfork_hub_tests PRIVATE soundfork_warnings)
add_dependencies(soundfork_hub_tests soundfork_capture_hub)

if(SOUNDFORK_BUILD_OBS_PLUGIN)
  include(cmake/ObsPlugin.cmake)
endif()

add_executable(soundfork_relay_tests
  tests/relay_clock_tests.cpp
  src/capture-backend/relay_clock.cpp
  src/capture-backend/shared_ring.cpp
)
target_include_directories(soundfork_relay_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(soundfork_relay_tests PRIVATE soundfork_warnings)

add_executable(soundfork_soak_tests
  tests/soak_tests.cpp
)
target_link_libraries(soundfork_soak_tests PRIVATE
  soundfork_bridge
  soundfork_fake_backend
  soundfork_warnings
  psapi
)
target_include_directories(soundfork_host PRIVATE "${SOUNDFORK_SDK_DIR}")
target_include_directories(soundfork_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(soundfork_host PRIVATE
  soundfork_bridge
  soundfork_warnings
  ole32
  advapi32
)

add_library(soundfork_obs_manager STATIC src/control-app/obs_plugin_manager.cpp)
target_include_directories(soundfork_obs_manager PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(soundfork_obs_manager PUBLIC soundfork_warnings version shell32 ole32 uuid)

add_library(soundfork_relay_settings STATIC src/control-app/relay_settings.cpp)
target_include_directories(soundfork_relay_settings PUBLIC src)
target_link_libraries(soundfork_relay_settings PUBLIC soundfork_warnings advapi32)

add_executable(soundfork_control WIN32
  src/control-app/control_main.cpp
  src/control-app/update_checker.cpp
  src/control-app/soundfork_control.rc
  "${CMAKE_CURRENT_BINARY_DIR}/generated/soundfork_version.rc"
  src/capture-backend/shared_ring.cpp
)
target_include_directories(soundfork_control PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(soundfork_control PRIVATE
  soundfork_obs_manager
  soundfork_relay_settings
  soundfork_warnings
  ole32
  uuid
  advapi32
  shell32
  winhttp
)

if(SOUNDFORK_BUILD_OBS_PLUGIN)
  target_compile_definitions(soundfork_control PRIVATE SOUNDFORK_EXPECT_OBS_PAYLOAD)
  add_dependencies(soundfork_control soundfork_obs_bundle)
endif()

if(BUILD_TESTING)
  add_executable(soundfork_relay_settings_tests tests/relay_settings_tests.cpp)
  target_link_libraries(soundfork_relay_settings_tests PRIVATE soundfork_relay_settings ole32)
  add_dependencies(soundfork_relay_settings_tests soundfork_control)
  add_test(NAME soundfork_relay_settings_tests COMMAND soundfork_relay_settings_tests "$<TARGET_FILE:soundfork_control>")
  set_tests_properties(soundfork_relay_settings_tests PROPERTIES TIMEOUT 20)
  add_executable(soundfork_obs_manager_tests tests/obs_plugin_manager_tests.cpp)
  target_link_libraries(soundfork_obs_manager_tests PRIVATE soundfork_obs_manager)
  add_dependencies(soundfork_obs_manager_tests soundfork_control)
  add_test(NAME soundfork_obs_manager_tests COMMAND soundfork_obs_manager_tests "$<TARGET_FILE:soundfork_control>")
endif()

if(BUILD_TESTING)
  add_test(NAME soundfork_proxy_tests COMMAND soundfork_tests)
  add_test(
    NAME soundfork_com_server_tests
    COMMAND soundfork_com_tests $<TARGET_FILE:soundfork_driver>
  )
  add_test(NAME soundfork_host_list COMMAND soundfork_host --list)
  add_test(NAME soundfork_capture_ring_tests COMMAND soundfork_capture_tests)
  add_test(NAME soundfork_capture_hub_tests COMMAND soundfork_hub_tests)
  set_tests_properties(soundfork_capture_hub_tests PROPERTIES TIMEOUT 30)
  add_test(NAME soundfork_relay_clock_tests COMMAND soundfork_relay_tests)
  add_test(
    NAME soundfork_discord_relay_list
    COMMAND soundfork_discord_relay --list-endpoints
  )
  add_test(NAME soundfork_control_smoke COMMAND soundfork_control --self-test)
endif()

install(TARGETS
  soundfork_driver
  soundfork_host
  soundfork_capture_monitor
  soundfork_discord_relay
  soundfork_capture_hub
  soundfork_control
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION bin
)
install(FILES
  README.md
  README.en.md
  LICENSE
  THIRD_PARTY_NOTICES.md
  THIRD_PARTY_NOTICES.en.md
  DESTINATION .
)
install(FILES
  docs/DETAILS.md
  docs/DETAILS.en.md
  docs/DEVELOPMENT.md
  docs/DEVELOPMENT.en.md
  DESTINATION docs
)
install(FILES
  assets/soundfork.ico
  DESTINATION assets
)
install(PROGRAMS
  scripts/Get-SoundForkStatus.ps1
  scripts/Install-SoundFork.ps1
  scripts/Register-SoundFork.ps1
  scripts/Remove-SoundFork.ps1
  scripts/Start-SoundForkRelay.ps1
  scripts/Stop-SoundForkRelay.ps1
  scripts/Test-SoundForkSoak.ps1
  scripts/Unregister-SoundFork.ps1
  scripts/Watch-SoundForkHardwareSoak.ps1
  DESTINATION scripts
)
