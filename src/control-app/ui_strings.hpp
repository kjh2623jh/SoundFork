#pragma once

// Text-only localization; independent of Win32 widgets and live settings.
namespace soundfork::control {

enum class UiLanguage {
  english,
  korean,
};

struct UiStrings {
  const wchar_t* window_title;
  const wchar_t* settings_menu;
  const wchar_t* language_menu;
  const wchar_t* help_menu;
  const wchar_t* check_for_updates;
  const wchar_t* open_project;
  const wchar_t* about;
  const wchar_t* update_available;
  const wchar_t* update_current;
  const wchar_t* update_open_question;
  const wchar_t* update_not_available;
  const wchar_t* update_failed;
  const wchar_t* open_project_failed;
  const wchar_t* about_license;
  const wchar_t* tray_open;
  const wchar_t* tray_exit_control;
  const wchar_t* tray_stop_and_exit;
  const wchar_t* tray_hidden_title;
  const wchar_t* tray_hidden_message;
  const wchar_t* backend_label;
  const wchar_t* apply;
  const wchar_t* endpoint_label;
  const wchar_t* start_relay;
  const wchar_t* stop_relay;
  const wchar_t* refresh_devices;
  const wchar_t* get_vb_cable;
  const wchar_t* note;
  const wchar_t* recommended;
  const wchar_t* supported;
  const wchar_t* ready;
  const wchar_t* not_ready;
  const wchar_t* not_configured;
  const wchar_t* not_registered;
  const wchar_t* asio_driver;
  const wchar_t* backend;
  const wchar_t* driver_dll;
  const wchar_t* asio_stream;
  const wchar_t* active;
  const wchar_t* inactive;
  const wchar_t* format;
  const wchar_t* channels;
  const wchar_t* frames_per_block;
  const wchar_t* published_consumed;
  const wchar_t* dropped_blocks;
  const wchar_t* capture_consumer;
  const wchar_t* none;
  const wchar_t* broadcast;
  const wchar_t* broadcast_not_ready;
  const wchar_t* discord_relay;
  const wchar_t* running;
  const wchar_t* stopped;
  const wchar_t* discord_mode;
  const wchar_t* application_share_note;
  const wchar_t* relay_missing;
  const wchar_t* relay_start_failed;
  const wchar_t* relay_stop_missing;
  const wchar_t* relay_stop_failed;
  const wchar_t* select_backend;
  const wchar_t* backend_updated;
  const wchar_t* backend_update_failed;
  const wchar_t* another_consumer;
  const wchar_t* select_endpoint;
  const wchar_t* open_vb_cable_failed;
  const wchar_t* com_initialization_failed;
  const wchar_t* backend_missing;
  const wchar_t* backend_access_denied;
  const wchar_t* backend_write_failed;
};

const UiStrings& ui_strings(UiLanguage language);

}  // namespace soundfork::control
