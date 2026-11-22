#pragma once
#include "canvas.hpp"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>
namespace usage_panel::zltech {
/**
 * ST7789 panel on SPI3 (240 x 240, 16 bit colour).
 *
 * The canvas holds the frame in RGB565 already, so a frame is sent as it stands: no expansion
 * pass, and every ink is a colour on the glass.
 *
 * The frame goes out in horizontal bands, each taken straight from the PSRAM frame and awaited
 * in turn. The wait is what keeps the panel-io queue from backing up, and a band that never
 * arrived is what used to leave a row of the previous frame behind.
 */
class St7789 {
  public:
    static constexpr int kFrameBytes = Canvas::kPixels * 2;
    // Band height. One transfer is 9.6 KB, about 2 ms on the wire, so the panel-io pipeline
    // copies it through its own internal buffer without holding a whole frame there.
    static constexpr int kBandRows = 20;

    /** Brings up the backlight, SPI3 and the controller. @return false on the first error. */
    bool begin();
    /** Sets the backlight in percent, clamped to [0, 100]. */
    void set_brightness(int percent);
    /**
     * Transfers `canvas` unless it matches the last submitted frame.
     * Every band is awaited before the next is queued.
     * @return true when the frame was sent; false when unchanged or failed.
     */
    bool show(const Canvas &canvas);

  private:
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    uint16_t *displayed_ = nullptr; // Copy of the last submitted frame, in PSRAM.
    SemaphoreHandle_t done_ = nullptr;
    bool has_frame_ = false;
    bool backlight_ready_ = false;
    static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event,
                              void *ctx);
};
} // namespace usage_panel::zltech
