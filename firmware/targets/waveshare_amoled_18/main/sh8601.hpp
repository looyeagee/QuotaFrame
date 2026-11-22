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
 * The frame goes out in horizontal strips, each with its own address window: one 322 KiB
 * transfer would be a very long DMA chain across PSRAM, and a short window per strip keeps
 * every transaction bounded. The wait on the colour callback is what keeps a strip from being
 * queued on top of one still on the wire. Brightness is the panel's own CABC command: the
 * SH8601 has no backlight pin, so the AMOLED current is set over the command channel.
 *
 * The canvas is RGB565 already, so a frame is sent as the canvas holds it. The last submitted
 * frame is kept beside it: on an AMOLED a frame that changes nothing still costs a refresh of
 * the whole glass, and the panel is the most expensive thing on the board to light up.
 */
class Sh8601 {
  public:
    static constexpr int kFrameBytes = Canvas::kNativeWidth * Canvas::kNativeHeight * 2;
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
    uint16_t *displayed_ = nullptr; // Copy of the last submitted frame, in PSRAM.
    SemaphoreHandle_t done_ = nullptr;
    static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event,
                              void *ctx);
};
} // namespace usage_panel::amoled
