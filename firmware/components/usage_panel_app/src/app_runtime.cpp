#include "usage_panel/app_runtime.hpp"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "usage_ota/presentation.hpp"

#include <cstdio>

namespace usage_panel {
namespace {
// Frames are composed at a fixed cadence so countdowns tick; unchanged frames are not sent.
constexpr uint64_t kFrameIntervalMs = 250;
// How long the panel keeps the banner of a failed update on screen.
constexpr uint64_t kOtaErrorVisibleMs = 15000;

static_assert(static_cast<int>(OtaPhase::Rebooting) == static_cast<int>(OtaStage::Rebooting));
static_assert(warning_level(0, 80) == 1 && warning_level(1, 78) == 1 && warning_level(1, 77) == 0);
static_assert(warning_level(0, 95) == 2 && warning_level(2, 93) == 2 && warning_level(2, 92) == 1);
} // namespace

PanelRuntime::PanelRuntime(PanelTarget &target, const PanelIdentity &identity)
    : target_(target), identity_(identity), backend_(identity.ota_project), ota_(backend_) {}

const char *PanelRuntime::page_name(void *context) {
    const auto *runtime = static_cast<const PanelRuntime *>(context);
    switch (runtime->location_.load()) {
    case Page::Deepseek:
        return "deepseek";
    case Page::Minimax:
        return "minimax";
    case Page::Sessions:
        return "sessions";
    case Page::Music:
        return "music";
    case Page::Clock:
        return "clock";
    case Page::Settings:
        return "settings";
    default:
        return "overview";
    }
}

uint8_t PanelRuntime::alert_mode(void *context) {
    // Read from the BLE callback; a single byte is published without tearing.
    const auto *runtime = static_cast<const PanelRuntime *>(context);
    return runtime->view_.settings.alert;
}

uint8_t PanelRuntime::brightness_value(void *context) {
    // Read from the BLE callback; a single byte is published without tearing.
    const auto *runtime = static_cast<const PanelRuntime *>(context);
    return runtime->view_.settings.brightness;
}

void PanelRuntime::apply_ota(const OtaSnapshot &s) {
    view_.ota.stage = static_cast<OtaStage>(s.phase);
    view_.ota.offset = s.offset;
    view_.ota.size = s.size;
    snprintf(view_.ota.version, sizeof(view_.ota.version), "%s", s.version.c_str());
}

void PanelRuntime::run() {
    // Preserve NVS on initialization failures; recovery must not silently erase bonds.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(target_.begin() ? ESP_OK : ESP_FAIL);
    view_.settings = target_.load();
    // A panel without a dimmer has no brightness row to show or to step through.
    view_.setting_rows =
        static_cast<uint8_t>(identity_.dimmable ? kSettingRows : kSettingRowsNoDimmer);
    // The panel boots at its own default; the persisted brightness takes over here.
    target_.apply_brightness(view_.settings.brightness);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(view_.device_name, sizeof(view_.device_name), "%s%02X%02X", identity_.advertising_prefix,
             mac[4], mac[5]);
    view_.now_ms = esp_timer_get_time() / 1000;
    target_.poll(view_);
    ESP_ERROR_CHECK(events_.begin(32) ? ESP_OK : ESP_ERR_NO_MEM);
    // No OTA power source is registered: the board senses battery voltage only and cannot tell
    // whether USB power is present, so a voltage threshold would block updates on USB power.
    BleServiceConfig config{identity_.advertising_prefix, identity_.status_name,
                            identity_.bridge_target, true, &ota_, page_name, this, true, true,
                            true, identity_.sessions_page, true, alert_mode, this,
                            identity_.dimmable, brightness_value, this};
    ESP_ERROR_CHECK(ble_.start(events_, config));
    run_loop();
}

void PanelRuntime::run_loop() {
    uint64_t next_poll = 0, next_frame = 0, reboot_at = 0;
    RetainedOtaFailure shown_failure;
    Page clock_return = Page::Home;
    while (true) {
        view_.now_ms = esp_timer_get_time() / 1000;
        bool redraw = false;
        ota_.tick(view_.now_ms);
        const OtaSnapshot snapshot = ota_.snapshot();
        apply_ota(snapshot);
        if (retain_ota_failure(snapshot, shown_failure, view_.now_ms))
            redraw = true;
        view_.ota.error = shown_failure.error != OtaError::None &&
                                 view_.now_ms - shown_failure.started_ms < kOtaErrorVisibleMs
                             ? ota_error_presentation(shown_failure.error).title
                             : nullptr;
        if (snapshot.phase == OtaPhase::Rebooting) {
            // Leave the RESTARTING frame on the panel briefly before the reset.
            if (!reboot_at)
                reboot_at = view_.now_ms + 1000;
            else if (view_.now_ms >= reboot_at)
                esp_restart();
        }

        // The now-playing publication is a snapshot: the newest one replaces the last.
        NowPlayingUpdate music;
        if (ble_.take_now_playing(music)) {
            apply_now_playing(view_, music, view_.now_ms);
            redraw = true;
        }

        // The sessions publication is the same kind of snapshot: the newest
        // roster replaces the previous one, and the first one is what puts the
        // page into the ring.
        if (ble_.take_sessions(pending_sessions_)) {
            apply_sessions(view_, pending_sessions_);
            redraw = true;
        }

        AppEvent event{};
        while (events_.receive(event)) {
            switch (event.type) {
            case AppEventType::Usage:
                view_.model.apply(event.usage, view_.now_ms);
                break;
            case AppEventType::Link:
                view_.link = {event.link.connected, event.link.encrypted, event.link.has_passkey,
                              event.link.passkey};
                break;
            case AppEventType::TimeSync:
                if (!target_.sync(event.time_sync.calendar))
                    ESP_LOGW(identity_.tag, "clock sync rejected");
                next_poll = 0;
                break;
            case AppEventType::ScreenToggle:
                if (!ota_.busy() && !view_.link.has_passkey) {
                    if (view_.page == Page::Clock)
                        view_.page = clock_return;
                    else {
                        clock_return = view_.page == Page::Settings ? Page::Home : view_.page;
                        view_.page = Page::Clock;
                    }
                }
                break;
            case AppEventType::ScreenPage:
                if (!ota_.busy() && !view_.link.has_passkey && view_.page != Page::Settings) {
                    view_.page = event.previous_page ? Navigator::previous(view_)
                                                    : Navigator::next(view_);
                }
                break;
            case AppEventType::AlertSet:
                // Remote and local changes write the same persisted value; a failed NVS commit
                // leaves the old mode in force and surfaces RETRY. Pairing does not block this:
                // the mode only affects a later alert.
                if (!ota_.busy()) {
                    view_.settings.alert = event.alert_mode;
                    view_.save_error = !target_.save(view_.settings);
                }
                break;
            case AppEventType::BrightnessSet:
                // The same persistence contract as AlertSet: the percent lands in NVS and on
                // the panel, and a rejected write keeps the previous brightness in force.
                if (!ota_.busy()) {
                    view_.settings.brightness = event.brightness;
                    view_.save_error = !target_.save(view_.settings);
                    target_.apply_brightness(view_.settings.brightness);
                }
                break;
            }
        }

        {
            // Only MiniMax publishes a quota window, so only it raises an alert.
            const auto &s = view_.model.snapshot(Provider::Minimax);
            view_.warning[1] = short_present(s, view_.link.encrypted)
                                  ? warning_level(view_.warning[1], s.short_window.used_percent)
                                  : 0;
            view_.warning[0] = 0;
        }
        const int previous_alert = view_.alert;
        if (nav_.update_alert(view_) &&
            view_.settings.alert == static_cast<uint8_t>(AlertMode::Beep))
            target_.beep();
        redraw = redraw || previous_alert != view_.alert;

        const Input input = target_.key(view_.now_ms);
        if (input != Input::None) {
            redraw = true;
            if (snapshot.phase == OtaPhase::Confirming) {
                // Any key confirms the update; holding a key denies it.
                if (input == Input::Hold)
                    ota_.deny();
                else
                    ota_.confirm(view_.now_ms);
            } else if (ota_.busy()) {
            } else if (view_.link.has_passkey) {
                if (input != Input::Hold)
                    ble_.reset_pairing();
            } else {
                const NavResult r = nav_.input(input, view_, view_.now_ms);
                if (r.save) {
                    view_.save_error = !target_.save(view_.settings);
                    // The Settings page may have changed the brightness; every other row
                    // writes the same value back, which the panel treats as a no-op.
                    target_.apply_brightness(view_.settings.brightness);
                }
            }
        }
        if (!ota_.busy() && !view_.link.has_passkey)
            redraw = nav_.auto_cycle(view_, view_.now_ms) || redraw;

        if (view_.now_ms >= next_poll) {
            next_poll = view_.now_ms + 1000;
            target_.poll(view_);
            redraw = true;
        }
        location_ = view_.page;
        // The song reaches its next line between two publications; compose the frame then,
        // instead of waiting for the quarter-second frame boundary that only paces countdowns.
        redraw = redraw || music_line_moved(view_, view_.now_ms);
        // Frames are composed at a fixed cadence so countdowns tick; unchanged frames are not sent.
        if (redraw || view_.now_ms >= next_frame) {
            view_.music_line_shown = music_current_line(view_, view_.now_ms);
            target_.render(view_);
            next_frame = view_.now_ms + kFrameIntervalMs;
        }
        // BLE startup is asynchronous; verification retries until the service is ready.
        ble_.verify_boot();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

} // namespace usage_panel
