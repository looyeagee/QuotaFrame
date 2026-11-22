#include "st7789.hpp"
#include "board_config.hpp"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
namespace usage_panel::zltech {
namespace {
constexpr char kTag[] = "st7789";
// 1 bit of the canvas is 1 = black, so a lit bit expands to black and a clear bit to white.
constexpr uint16_t kBlack = 0x0000, kWhite = 0xFFFF;
// One band is 9.6 KB, about 2 ms on the wire at 40 MHz; the timeout only catches a stuck bus.
constexpr uint32_t kBandTimeoutMs = 100;
} // namespace

bool St7789::on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx) {
    auto *self = static_cast<St7789 *>(ctx);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->done_, &woken);
    return woken == pdTRUE;
}

bool St7789::begin() {
    // Backlight first: the panel shows nothing until the LEDC channel drives its pin.
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = kBacklightTimer;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK)
        return false;
    ledc_channel_config_t channel{};
    channel.gpio_num = kLcdBacklight;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = kBacklightChannel;
    channel.timer_sel = kBacklightTimer;
    channel.duty = 0;
    channel.hpoint = 0;
    if (ledc_channel_config(&channel) != ESP_OK)
        return false;
    backlight_ready_ = true;

    spi_bus_config_t bus{};
    bus.sclk_io_num = kLcdSclk;
    bus.mosi_io_num = kLcdMosi;
    bus.miso_io_num = -1;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = St7789::kBandBytes;
    if (spi_bus_initialize(static_cast<spi_host_device_t>(kLcdSpiHost), &bus, SPI_DMA_CH_AUTO) !=
        ESP_OK)
        return false;

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = kLcdCs;
    io_config.dc_gpio_num = kLcdDc;
    io_config.spi_mode = 0;
    io_config.pclk_hz = kLcdPclkHz;
    // One slot is all the pipeline needs: each band is awaited before the next is submitted.
    io_config.trans_queue_depth = 1;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kLcdSpiHost), &io_config,
                                 &io_) != ESP_OK)
        return false;

    done_ = xSemaphoreCreateBinary();
    if (!done_)
        return false;
    esp_lcd_panel_io_callbacks_t callbacks{};
    callbacks.on_color_trans_done = on_color_done;
    if (esp_lcd_panel_io_register_event_callbacks(io_, &callbacks, this) != ESP_OK)
        return false;

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = kLcdRst;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
    panel_config.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(io_, &panel_config, &panel_) != ESP_OK)
        return false;
    if (esp_lcd_panel_reset(panel_) != ESP_OK || esp_lcd_panel_init(panel_) != ESP_OK)
        return false;
    // The panel's colour inversion matches the vendor image this board shipped with.
    esp_lcd_panel_invert_color(panel_, true);
    esp_lcd_panel_swap_xy(panel_, false);
    esp_lcd_panel_mirror(panel_, false, false);
    esp_lcd_panel_disp_on_off(panel_, true);

    // The band buffer is internal DMA memory; 9.6 KB leaves the NimBLE heap it needs at startup.
    band_ = static_cast<uint16_t *>(
        heap_caps_malloc(kBandBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    displayed_ = static_cast<uint8_t *>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_8BIT));
    if (!band_ || !displayed_) {
        ESP_LOGE(kTag, "frame buffers unavailable");
        return false;
    }
    memset(displayed_, 0, kFrameBytes);
    set_brightness(kBacklightPercent);
    ESP_LOGI(kTag, "panel ready %dx%d, %d-row bands, backlight %d%%", kPanelWidth, kPanelHeight,
             kBandRows, kBacklightPercent);
    return true;
}

void St7789::set_brightness(int percent) {
    if (!backlight_ready_)
        return;
    const int clamped = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    // A 10 bit duty keeps the low end of the range usable at indoor brightness.
    const uint32_t duty = static_cast<uint32_t>(clamped) * 1023 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel);
}

void St7789::expand(const Canvas &canvas, int first_row, int rows) {
    const uint8_t *bits = canvas.data();
    for (int r = 0; r < rows; ++r) {
        const int y = first_row + r;
        const uint8_t *line = bits + (y * Canvas::kNativeWidth) / 8;
        uint16_t *out = band_ + r * Canvas::kNativeWidth;
        for (int x = 0; x < Canvas::kNativeWidth; ++x) {
            const bool black = (line[x >> 3] >> (7 - (x & 7))) & 1;
            out[x] = black ? kBlack : kWhite;
        }
    }
}

bool St7789::show(const Canvas &canvas) {
    if (!panel_)
        return false;
    if (has_frame_ && memcmp(displayed_, canvas.data(), kFrameBytes) == 0)
        return false;
    for (int y = 0; y < kPanelHeight; y += kBandRows) {
        expand(canvas, y, kBandRows);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, y, kPanelWidth, y + kBandRows, band_) != ESP_OK) {
            ESP_LOGW(kTag, "band at y=%d rejected", y);
            return false;
        }
        // Awaiting the band is what keeps the panel-io queue from backing up mid-frame.
        if (xSemaphoreTake(done_, pdMS_TO_TICKS(kBandTimeoutMs)) == pdFALSE) {
            ESP_LOGW(kTag, "band at y=%d timed out", y);
            return false;
        }
    }
    memcpy(displayed_, canvas.data(), kFrameBytes);
    has_frame_ = true;
    return true;
}
} // namespace usage_panel::zltech
