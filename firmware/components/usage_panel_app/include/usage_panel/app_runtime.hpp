#pragma once

#include "usage_panel/panel_logic.hpp"
#include "usage_panel/view.hpp"
#include "usage_ble/app_events.hpp"
#include "usage_ble/ble_service.hpp"
#include "usage_ota/backend.hpp"
#include "usage_ota/session.hpp"
#include "usage_protocol/time_sync.hpp"

#include <atomic>
#include <cstdint>

namespace usage_panel {

/**
 * The hardware and the screens a target contributes to the shared runtime.
 *
 * Everything that is not the same on every board lives behind this interface: the
 * peripherals, the display, the keys, the settings store and the screen layout.
 */
class PanelTarget {
  public:
    virtual ~PanelTarget() = default;

    /** Brings up the board, the display and the alert path. */
    virtual bool begin() = 0;
    /** Reads the persisted settings. */
    virtual Settings load() = 0;
    /** Writes the settings; false when the store rejected the write. */
    virtual bool save(const Settings &settings) = 0;
    /** Reads the clock, the battery and the sensors into `view`. */
    virtual void poll(View &view) = 0;
    /** Anchors the clock at `time`; false when the value is not a usable date. */
    virtual bool sync(const LocalCalendarTime &time) = 0;
    /** The debounced key action; Input::None when no key was pressed. */
    virtual Input key(uint64_t now_ms) = 0;
    /** Composes the frame for `view` and pushes it to the panel. */
    virtual void render(const View &view) = 0;
    /** Plays the alert sound. */
    virtual void beep() = 0;
    /** Applies the display brightness percent to the panel hardware. */
    virtual void apply_brightness(uint8_t percent) = 0;
};

/** The identity a target publishes to the Bridge and to the OTA backend. */
struct PanelIdentity {
    const char *tag;                // Log tag of the target.
    const char *advertising_prefix; // BLE name prefix; the MAC suffix is appended.
    const char *status_name;        // Device name the Bridge shows.
    const char *bridge_target;      // Target id the Bridge status reports.
    const char *ota_project;        // Project name the OTA image has to carry.
    // Whether this target renders the sessions page. Only a target that draws
    // it may declare the capability, or the Bridge would publish to screens
    // nothing renders.
    bool sessions_page = false;
    // Whether the panel has a dimmer. A reflective panel without a backlight
    // hides the brightness setting and does not declare brightness.set.v1, so
    // the Bridge never offers a control the glass cannot show.
    bool dimmable = true;
};

/**
 * The application every panel target runs.
 *
 * The runtime owns the link, the OTA session, the view and the navigation; the target owns
 * the hardware and the screens. The loop is the same on every board, so it lives here.
 */
class PanelRuntime {
  public:
    /**
     * Binds the runtime to a target and to the identity the panel publishes.
     *
     * The identity strings have to outlive the runtime: the OTA backend keeps the project
     * name it checks the image against.
     */
    PanelRuntime(PanelTarget &target, const PanelIdentity &identity);

    /** Brings the panel up and runs the event loop. It never returns. */
    void run();

  private:
    PanelTarget &target_;
    PanelIdentity identity_;
    AppEventQueue events_;
    BleService ble_;
    EspOtaBackend backend_;
    OtaSession ota_;
    Navigator nav_;
    View view_;
    std::atomic<Page> location_{Page::Home};

    static const char *page_name(void *context);
    static uint8_t alert_mode(void *context);
    static uint8_t brightness_value(void *context);
    void apply_ota(const OtaSnapshot &snapshot);
    void run_loop();
};

} // namespace usage_panel
