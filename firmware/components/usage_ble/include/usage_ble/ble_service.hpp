#pragma once

#include <atomic>

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/folder_push.h"
#include "esp_desktop_buddy/transport_ble.h"
#include "usage_ble/app_events.hpp"
#include "usage_ble/now_playing_mailbox.hpp"
#include "usage_ble/ota_commands.hpp"
#include "usage_ble/sessions_mailbox.hpp"
#include "usage_ota/folder_push_sink.hpp"
#include "usage_ota/session.hpp"

namespace usage_panel {

/** Returns the current public UI location, such as overview/codex/claude. */
using UiLocationGetter = const char* (*)(void*);

/** Returns the persisted alert mode (0 off, 1 alert page, 2 page and beep). */
using AlertModeGetter = uint8_t (*)(void*);

/** Returns the persisted display brightness in percent (0..100). */
using BrightnessGetter = uint8_t (*)(void*);

/** Configuration copied or retained by BleService::start(). */
struct BleServiceConfig {
    // Identity strings are copied into fixed service-owned buffers during start.
    const char* advertising_prefix;
    const char* status_name;
    const char* target;
    bool enable_time_sync;
    // The OTA session, callback, and callback context must outlive BleService.
    OtaSession* ota;
    UiLocationGetter get_ui_location;
    void* ui_context;
    bool enable_screen_toggle = false;
    bool enable_screen_page = false;
    bool enable_now_playing = false;
    // Advertises sessions.v1 and the extended states of sessions.v2; a target
    // without a sessions page must not declare them, or the Bridge would send
    // frames nothing draws.
    bool enable_sessions = false;
    // Advertises alert.set.v1; the getter reports the value saved by the device so
    // status and the remote setting cannot drift apart.
    bool enable_alert_set = false;
    AlertModeGetter get_alert_mode = nullptr;
    void* alert_context = nullptr;
    // Advertises brightness.set.v1 with the same status-drift guarantee as alert.set.v1.
    bool enable_brightness_set = false;
    BrightnessGetter get_brightness = nullptr;
    void* brightness_context = nullptr;
};

/**
 * Validates and queues a time-sync command, returning a protocol ACK.
 * Disabled support is reported as `unsupported`; a full application queue is
 * reported as `queue_full` rather than blocking the BLE callback.
 */
esp_desktop_buddy_command_extension_result_t route_time_sync_command(
    const esp_desktop_buddy_command_view_t* request,
    bool enabled,
    AppEventQueue* queue);

/** Validates a screen toggle and queues it for the application loop. */
esp_desktop_buddy_command_extension_result_t route_screen_toggle_command(
    const esp_desktop_buddy_command_view_t* request,
    bool enabled, bool ota_busy, AppEventQueue* queue);

/** Queues a single relative page change; OTA prevents navigation. */
esp_desktop_buddy_command_extension_result_t route_screen_page_command(
    const esp_desktop_buddy_command_view_t* request,
    bool enabled, bool ota_busy, AppEventQueue* queue);

/**
 * Validates alert_set mode 0..2 and queues it for the application loop.
 * OTA owns the device while receiving, so a setting change is refused then.
 */
esp_desktop_buddy_command_extension_result_t route_alert_set_command(
    const esp_desktop_buddy_command_view_t* request,
    bool enabled, bool ota_busy, AppEventQueue* queue);

/**
 * Validates brightness_set percent 0..100 and queues it for the application loop.
 * OTA owns the device while receiving, so a setting change is refused then.
 */
esp_desktop_buddy_command_extension_result_t route_brightness_set_command(
    const esp_desktop_buddy_command_view_t* request,
    bool enabled, bool ota_busy, AppEventQueue* queue);

/**
 * Owns the authenticated BLE transport and routes Buddy commands to app state.
 *
 * The service requires bonding, MITM protection, Secure Connections, and a
 * DisplayOnly passkey ceremony. A link is usable only when both `connected`
 * and `encrypted` are true.
 */
class BleService {
public:
    BleService() = default;
    explicit BleService(OtaSession& ota) : ota_(&ota), ota_commands_(ota) {}

    /** Creates the Buddy core, Folder Push adapter, and secure BLE transport. */
    esp_err_t start(AppEventQueue& queue, const BleServiceConfig& config);

    /** Call from the app loop after display initialization and the first render. */
    void verify_boot();

    /** Handles one validated usage command unless an OTA session is active. */
    esp_desktop_buddy_command_extension_result_t handle_usage(
        const esp_desktop_buddy_command_view_t* request);

    /** Stores one validated now-playing publication for the display loop. */
    esp_desktop_buddy_command_extension_result_t handle_now_playing(
        const esp_desktop_buddy_command_view_t* request);

    /** Takes the pending now-playing publication; false when none arrived. */
    bool take_now_playing(NowPlayingUpdate& out);

    /** Stores one validated sessions publication for the display loop. */
    esp_desktop_buddy_command_extension_result_t handle_sessions(
        const esp_desktop_buddy_command_view_t* request);

    /** Takes the pending sessions publication; false when none arrived. */
    bool take_sessions(SessionsUpdate& out);

    /** Aborts OTA state and rebuilds Folder Push protocol state on success. */
    esp_desktop_buddy_command_extension_result_t handle_ota_abort(
        const esp_desktop_buddy_command_view_t* request);

    esp_desktop_buddy_command_extension_result_t handle_time_sync(
        const esp_desktop_buddy_command_view_t* request);

    /** Returns current public capability/status data, including encryption state. */
    esp_desktop_buddy_status_reply_t status();

    /** Clears all stored BLE bonds for a device-local recovery action. */
    esp_desktop_buddy_command_result_t reset_pairing();

private:
    static esp_desktop_buddy_command_extension_result_t usage_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_command_extension_result_t now_playing_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_command_extension_result_t sessions_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_command_extension_result_t ota_abort_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_command_extension_result_t screen_toggle_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_command_extension_result_t screen_page_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_command_extension_result_t alert_set_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_command_extension_result_t brightness_set_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_command_extension_result_t time_sync_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_command_extension_result_t folder_push_trampoline(
        void*, esp_desktop_buddy_t*, const esp_desktop_buddy_command_view_t*);
    static esp_desktop_buddy_status_reply_t status_trampoline(
        void*, esp_desktop_buddy_t*);
    static void transport_trampoline(
        void*, const esp_desktop_buddy_transport_ble_event_t*);
    /** Current persisted alert mode for status, or -1 when the device reports none. */
    int alert_mode_value() const;
    /** Current persisted brightness percent for status, or -1 when the device reports none. */
    int brightness_value() const;
    bool reset_folder_push();

    AppEventQueue* queue_ = nullptr;
    UiLocationGetter get_ui_location_ = nullptr;
    void* ui_context_ = nullptr;
    esp_desktop_buddy_t* buddy_ = nullptr;
    esp_desktop_buddy_transport_ble_t* transport_ = nullptr;
    esp_desktop_buddy_folder_push_t* folder_push_ = nullptr;
    char advertising_prefix_[20]{};
    char advertising_name_[24]{};
    char status_name_[129]{};
    char target_[32]{};
    char status_json_[1024]{};
    std::atomic<bool> boot_valid_{false};
    int64_t startup_deadline_us_ = 0;
    bool enable_time_sync_ = false;
    bool enable_screen_toggle_ = false;
    bool enable_screen_page_ = false;
    bool enable_now_playing_ = false;
    bool enable_sessions_ = false;
    bool enable_alert_set_ = false;
    AlertModeGetter get_alert_mode_ = nullptr;
    void* alert_context_ = nullptr;
    bool enable_brightness_set_ = false;
    BrightnessGetter get_brightness_ = nullptr;
    void* brightness_context_ = nullptr;
    NowPlayingMailbox now_playing_{};
    SessionsMailbox sessions_{};
    OtaSession* ota_ = nullptr;
    OtaCommandRouter ota_commands_{};
    OtaFolderPushSink ota_folder_sink_{};
};

}  // namespace usage_panel
