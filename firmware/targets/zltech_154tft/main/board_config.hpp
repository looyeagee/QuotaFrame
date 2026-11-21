#pragma once
#include "driver/gpio.h"
#include "driver/ledc.h"

namespace usage_panel::zltech {
/**
 * Board pin map for the ZLTech 1.54 TFT Wi-Fi board (ESP32-S3, 240 x 240 ST7789).
 *
 * The map was recovered from the live GPIO matrix of the vendor image on the board itself:
 * SPI3 was routed to GPIO 9/10/14 and the backlight was driven from LEDC channel 0 on
 * GPIO 13, which matches the published pinout of the Star-Origin "cube 1.54 TFT Wi-Fi"
 * reference design this board is built from. The remaining pins follow that reference.
 */
inline constexpr gpio_num_t kLcdSclk = GPIO_NUM_9;   // SPI3_CLK
inline constexpr gpio_num_t kLcdMosi = GPIO_NUM_10;  // SPI3_D
inline constexpr gpio_num_t kLcdCs = GPIO_NUM_14;    // SPI3_CS0
inline constexpr gpio_num_t kLcdDc = GPIO_NUM_8;
inline constexpr gpio_num_t kLcdRst = GPIO_NUM_18;
inline constexpr gpio_num_t kLcdBacklight = GPIO_NUM_13; // LEDC channel 0

inline constexpr int kLcdSpiHost = 2;           // SPI3_HOST
inline constexpr int kLcdPclkHz = 40 * 1000 * 1000;
inline constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
inline constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;

/** Panel geometry and default backlight level (percent). */
inline constexpr int kPanelWidth = 240, kPanelHeight = 240;
inline constexpr int kBacklightPercent = 70;

/**
 * The three keys on the top edge of the board.
 *
 * The board follows the Star-Origin "cube 1.54 TFT Wi-Fi" reference design, whose keys are
 * GPIO0 (boot), GPIO39 (volume down) and GPIO40 (volume up). They are used as back, home and
 * forward: the left key steps back through the page ring, the middle key returns to the home
 * page and the right key steps forward. Holding any of them for 800 ms opens the settings.
 */
inline constexpr gpio_num_t kKeyBack = GPIO_NUM_39;     // VOL-, left key
inline constexpr gpio_num_t kKeyHome = GPIO_NUM_0;      // BOOT, middle key, also the strap pin
inline constexpr gpio_num_t kKeyForward = GPIO_NUM_40;  // VOL+, right key

/**
 * Battery sense: ADC2 channel 6, which is GPIO 17, through the board's divider.
 *
 * The live GPIO matrix showed GPIO 17 configured as an ADC input while the vendor image ran,
 * and GPIO 7 (ADC1 channel 6) is the speaker data line, so the sense must be on ADC2. The
 * board only ever runs Bluetooth, never Wi-Fi, so the ADC2/Wi-Fi conflict does not apply.
 */
inline constexpr int kBatteryChannel = 6;
inline constexpr float kBatteryDivider = 2.f;
} // namespace usage_panel::zltech
