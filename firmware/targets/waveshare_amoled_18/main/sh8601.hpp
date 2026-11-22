#pragma once
#include "canvas.hpp"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>
namespace usage_panel::amoled {
/**
 * SH8601 panel on QSPI (368 x 448, 16 bit colour).
 *
 * The board's glass is colour, but every screen in this firmware is a one-bit drawing, so a
 * frame is expanded from the canvas layout into black and white RGB565 pixels. On an AMOLED
 * a black pixel is an off sub-pixel, so the black background lights nothing and the drawing
 * carries the contrast on its own.
 *
 * The whole frame goes out in one transfer through the panel-io pipeline, which chunks it
 * into DMA-sized pieces on its own; the wait on the colour callback is what keeps a slow
 * frame from backing up behind the next one. Brightness is the panel's own CABC command:
 * the SH8601 has no backlight pin, so the AMOLED current is set over the command channel.
 */
class Sh8601 {
  public:
    static constexpr int kFrameBytes = Canvas::kBytes;
    static constexpr int kFramePixels = Canvas::kNativeWidth * Canvas::kNativeHeight;

    /** Brings up QSPI and the controller. @return false on the first error. */
    bool begin();
    /** Sets the panel brightness in percent, clamped to [0, 100]. */
    void set_brightness(int percent);
    /**
     * Transfers `canvas` unless it matches the last submitted frame.
     * @return true when the frame was sent; false when unchanged or failed.
     */
    bool show(const Canvas &canvas);

  private:
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    uint16_t *frame_ = nullptr;    // Full frame of RGB565 in PSRAM.
    uint8_t *displayed_ = nullptr; // Copy of the last submitted canvas.
    SemaphoreHandle_t done_ = nullptr;
    bool has_frame_ = false;
    void expand(const Canvas &canvas);
    static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event,
                              void *ctx);
};
} // namespace usage_panel::amoled
