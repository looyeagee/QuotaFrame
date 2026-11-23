#pragma once
#include "axp2101.hpp"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"
#include "usage_panel/panel_logic.hpp"
#include "usage_panel/view.hpp"
#include "usage_protocol/time_sync.hpp"
#include "usage_rtc/rtc_clock.hpp"
namespace usage_panel::amoled {
/**
 * Board peripherals other than the display: the touch panel, the BOOT key, the battery
 * gauge and the clock.
 *
 * Page navigation on this board comes from the touch surface: a horizontal swipe steps
 * through the page ring, a tap returns home and a stationary hold opens the settings. A
 * vertical drag scrolls the list of a page that is longer than the glass, pixel by pixel.
 * The BOOT key mirrors the tap and the hold for those moments when the glass is out of
 * reach. The gesture directions follow the rotation setting, so "next page" stays leftwards
 * on the glass in every orientation.
 */
class Board {
  public:
    /** Brings up I2C, the expander power rails, RTC, PMIC, touch and the BOOT key. */
    bool begin();
    /** Reads the clock every call and the battery every 10 s. */
    void poll(View &view);
    /** Writes a synced calendar into the RTC. */
    bool sync(const LocalCalendarTime &time);
    /**
     * The debounced input of the touch surface and the BOOT key.
     *
     * A horizontal swipe of at least 60 px that outruns its vertical travel steps to the
     * next (leftwards) or previous (rightwards) page in the current orientation. A vertical
     * drag scrolls the list of the page that shows one: the distance the finger covered is
     * reported as it happens, so the list follows the finger pixel by pixel. A short tap
     * without travel is the home action, a hold of 800 ms is the hold action, and a second
     * touch point cancels the gesture instead of guessing.
     */
    PanelInput read_input(uint64_t now_ms);
    bool save(const Settings &settings);
    Settings load();

  private:
    /** Tracks one active touch across the polls of `read_input`. */
    struct Gesture {
        bool down = false;
        bool multi = false;     // More than one touch point was seen during the stroke.
        bool hold_reported = false;
        bool scrolling = false; // The stroke was taken for a scroll; the list moves with it.
        bool paged = false;     // The stroke was taken for a page swipe.
        uint16_t start_x = 0, start_y = 0, last_x = 0, last_y = 0;
        // The movement since the last reported scroll, in native pixels.
        int pending_x = 0, pending_y = 0;
        uint64_t start = 0;
    };
    /** Maps a native-panel touch delta into the logical space the pages are drawn in. */
    void logical_delta(int &dx, int &dy) const;
    i2c_master_bus_handle_t bus_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    Axp2101 pmic_;
    RtcClock rtc_;
    bool rtc_ready_ = false;
    uint8_t rotation_ = 0; // Rotation setting, mirrored from load/save for swipe mapping.
    uint64_t battery_at_ = 0;
    Gesture gesture_;
    struct Key {
        bool raw = false, down = false, held = false;
        uint64_t changed = 0, start = 0;
    };
    Key boot_;
    void read_battery(View &view);
    PanelInput poll_touch(uint64_t now_ms);
    PanelInput poll_key(uint64_t now_ms);
};
} // namespace usage_panel::amoled
