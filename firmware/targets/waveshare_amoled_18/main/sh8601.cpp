#include "sh8601.hpp"
#include "board_config.hpp"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/spi_master.h"
#include <algorithm>
#include <cstring>
namespace usage_panel::amoled {
namespace {
constexpr char kTag[] = "sh8601";
// One whole frame, in bytes, and the strip height a transfer is split into.
constexpr int kStripRows = 32;
// The wait only catches a stuck bus: a frame takes about 17 ms on the wire at 40 MHz.
constexpr uint32_t kFrameTimeoutMs = 200;
// Default panel brightness after bring-up, in percent; AMOLED full scale is blinding.
constexpr int kDefaultBrightness = 80;

// The esp_lcd_sh8601 defaults never wake the panel: its reset is SWRESET, which leaves the
// controller in sleep, and its vendor command list starts at 0x44. Without an explicit
// SLPOUT the glass stays dark while every transfer still "succeeds", which is why the
// sequence below mirrors the vendor's Arduino driver: wake, normal mode, no inversion,
// brightness control on, then the display-on command arrives from disp_on_off().
constexpr uint8_t kCtrlD1 = 0x28; // Brightness control on, no dimming.
static const sh8601_lcd_init_cmd_t kVendorInit[] = {
    {0x11, nullptr, 0, 120},          // SLPOUT
    {0x13, nullptr, 0, 0},            // NORON
    {0x20, nullptr, 0, 0},            // INVOFF
    {0x53, &kCtrlD1, 1, 10},          // CTRLD1
};
} // namespace

bool Sh8601::on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx) {
    auto *self = static_cast<Sh8601 *>(ctx);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->done_, &woken);
    return woken == pdTRUE;
}

bool Sh8601::begin() {
    // The vendor macros omit the fields IDF v6.1 added to the bus config, and this build
    // compiles with -Werror, so the QSPI bus is named field by field instead.
    spi_bus_config_t bus{};
    bus.sclk_io_num = kLcdSclk;
    bus.data0_io_num = kLcdData0;
    bus.data1_io_num = kLcdData1;
    bus.data2_io_num = kLcdData2;
    bus.data3_io_num = kLcdData3;
    // One strip per transfer: the bus never has to describe a whole frame at once.
    bus.max_transfer_sz = kStripRows * kPanelWidth * 2;
    if (spi_bus_initialize(static_cast<spi_host_device_t>(kLcdSpiHost), &bus, SPI_DMA_CH_AUTO) !=
        ESP_OK)
        return false;

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = kLcdCs;
    io_config.pclk_hz = kLcdPclkHz;
    // One slot is all the pipeline needs: each frame is awaited before the next is submitted.
    io_config.trans_queue_depth = 1;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.on_color_trans_done = on_color_done;
    io_config.user_ctx = this;
    io_config.flags.quad_mode = true;
    if (esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kLcdSpiHost), &io_config,
                                 &io_) != ESP_OK)
        return false;

    done_ = xSemaphoreCreateBinary();
    if (!done_)
        return false;

    // The reset line sits on the power-gating expander, so the controller gets a software
    // reset from the driver instead of a GPIO toggle.
    const sh8601_vendor_config_t vendor_config = {
        .init_cmds = kVendorInit,
        .init_cmds_size = sizeof(kVendorInit) / sizeof(kVendorInit[0]),
        .flags = {.use_qspi_interface = 1},
    };
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = const_cast<sh8601_vendor_config_t *>(&vendor_config);
    if (esp_lcd_new_panel_sh8601(io_, &panel_config, &panel_) != ESP_OK)
        return false;
    if (esp_lcd_panel_reset(panel_) != ESP_OK || esp_lcd_panel_init(panel_) != ESP_OK)
        return false;
    // No inversion: the SH8601 leaves the AMOLED panel with straight colour ordering. The
    // panel has no axis swap, and none is needed: the canvas draws every orientation into
    // the native buffer itself, so the controller always sees the 368 x 448 native space.
    esp_lcd_panel_mirror(panel_, false, false);
    esp_lcd_panel_disp_on_off(panel_, true);

    displayed_ = static_cast<uint16_t *>(
        heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!displayed_) {
        ESP_LOGE(kTag, "frame buffer unavailable");
        return false;
    }
    memset(displayed_, 0, kFrameBytes);
    set_brightness(kDefaultBrightness);
    ESP_LOGI(kTag, "panel ready %dx%d, brightness %d%%", kPanelWidth, kPanelHeight,
             kDefaultBrightness);
    return true;
}

void Sh8601::set_brightness(int percent) {
    if (!io_)
        return;
    const int clamped = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    // The brightness register is the panel's CABC command 0x51, sent in the same wrapped
    // form the SH8601 driver uses for every write on the QSPI interface.
    const uint8_t value = static_cast<uint8_t>(clamped * 255 / 100);
    const int wrapped = (0x02 << 24) | (0x51 << 8);
    esp_lcd_panel_io_tx_param(io_, wrapped, &value, 1);
}

bool Sh8601::show(const Canvas &canvas) {
    if (!panel_)
        return false;
    const uint16_t *frame = canvas.color_data();
    // A canvas that could not reserve its PSRAM buffer has nothing to send.
    if (frame == nullptr)
        return false;
    if (memcmp(displayed_, frame, kFrameBytes) == 0)
        return false;
    // A completion left over from an earlier frame must not be read as this strip's: the
    // semaphore is binary, and a stale token would let the next strip be queued while the
    // previous one is still on the wire, so the panel would latch command bytes as pixels.
    xSemaphoreTake(done_, 0);
    // One strip at a time, each with its own address window: a whole frame in a single transfer
    // is 322 KiB, a long DMA chain across PSRAM, and re-arming the window per strip keeps every
    // transaction short enough to reason about.
    for (int y = 0; y < kPanelHeight; y += kStripRows) {
        const int rows = std::min(kStripRows, kPanelHeight - y);
        if (esp_lcd_panel_draw_bitmap(panel_, 0, y, kPanelWidth, y + rows,
                                      frame + y * kPanelWidth) != ESP_OK) {
            ESP_LOGW(kTag, "strip %d rejected", y);
            return false;
        }
        if (xSemaphoreTake(done_, pdMS_TO_TICKS(kFrameTimeoutMs)) == pdFALSE) {
            ESP_LOGW(kTag, "strip %d timed out", y);
            return false;
        }
    }
    memcpy(displayed_, frame, kFrameBytes);
    return true;
}
} // namespace usage_panel::amoled
