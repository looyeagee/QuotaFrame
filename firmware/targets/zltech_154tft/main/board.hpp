#pragma once
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "panel_logic.hpp"
#include "usage_protocol/time_sync.hpp"
#include "view.hpp"
namespace usage_panel::zltech {
/**
 * Board peripherals other than the display: the three keys, the battery sense and the clock.
 *
 * This board has no RTC and no environment sensor. The clock is kept in RAM from the last
 * `usage.v1` time sync and advanced with the panel timer, so it is lost on reset and shows
 * NO DATE until the Bridge syncs again. The environment readings stay invalid, which the
 * screens render as placeholders rather than hiding the row.
 */
class Board {
  public:
    /** Configures the keys and the battery ADC. A missing ADC leaves the level unknown. */
    bool begin();
    /** Reads the clock every call and the battery every 10 s. */
    void poll(View &view);
    /** Anchors the software clock at `time`; false when the value is not a usable date. */
    bool sync(const LocalCalendarTime &time);
    /**
     * Debounced action of the three keys: back, home and forward.
     *
     * The action is reported when the key is released. Holding a key for 800 ms reports the
     * hold action instead, which opens the settings page.
     */
    Input key(uint64_t now_ms);
    bool save(const Settings &settings);
    Settings load();

  private:
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t cali_ = nullptr;
    bool clock_valid_ = false;
    int64_t clock_epoch_ = 0;   // Seconds since the Unix epoch at the last sync.
    uint64_t clock_at_ms_ = 0;  // Panel clock when that anchor was taken.
    uint64_t battery_at_ = 0;
    void read_battery(View &view);
};
} // namespace usage_panel::zltech
