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
 * The board's glass is colour, but every screen in this firmware is a one-bit drawing, so a
 * frame is expanded from the canvas layout into black and white RGB565 pixels.
 *
 * The frame goes out in horizontal bands through one small internal-DMA buffer. A whole-frame
 * buffer would be 115 KB of internal RAM, which is what NimBLE needs to start, so each band is
 * submitted and awaited in turn: the wait is what keeps the panel-io queue from backing up, and
 * a stale row of the previous frame is what a dropped band used to leave behind.
 */
class St7789 {
  public:
    static constexpr int kFrameBytes = Canvas::kBytes;
    static constexpr int kBandRows = 20;
    static constexpr int kBandBytes = Canvas::kNativeWidth * kBandRows * 2;

    /** Brings up the backlight, SPI3 and the controller. @return false on the first error. */
    bool begin();
    /** Sets the backlight in percent, clamped to [0, 100]. */
    void set_backlight(int percent);
    /**
     * Transfers `canvas` unless it matches the last submitted frame.
     * Every band is awaited before the next is composed, so a slow bus never leaves part of
     * the panel showing the previous frame.
     * @return true when the frame was sent; false when unchanged or failed.
     */
    bool show(const Canvas &canvas);

  private:
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    uint16_t *band_ = nullptr;      // kBandRows rows of RGB565 in internal DMA memory.
    uint8_t *displayed_ = nullptr;  // Copy of the last submitted frame.
    SemaphoreHandle_t done_ = nullptr;
    bool has_frame_ = false;
    bool backlight_ready_ = false;
    void expand(const Canvas &canvas, int first_row, int rows);
    static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event,
                              void *ctx);
};
} // namespace usage_panel::zltech
