#pragma once
#include "driver/gpio.h"

namespace usage_panel::amoled {
/**
 * Board pin map for the Waveshare ESP32-S3-Touch-AMOLED-1.8, V1 hardware.
 *
 * V1 carries the SH8601 AMOLED driver over QSPI and the FT3168 touch controller over I2C;
 * the V2 revision swaps both for CO5300/CST820 and is a different target. The panel and the
 * touch controller share the I2C bus with the AXP2101 PMIC, the PCF85063A RTC and a
 * TCA9554 expander that gates the display power rails.
 *
 * The map follows the vendor's first-party ESP-IDF examples for this board (display
 * colorbar demo and the BSP component `waveshare/esp32_s3_touch_amoled_1_8`).
 */
inline constexpr gpio_num_t kLcdSclk = GPIO_NUM_11;
inline constexpr gpio_num_t kLcdCs = GPIO_NUM_12;
inline constexpr gpio_num_t kLcdData0 = GPIO_NUM_4;
inline constexpr gpio_num_t kLcdData1 = GPIO_NUM_5;
inline constexpr gpio_num_t kLcdData2 = GPIO_NUM_6;
inline constexpr gpio_num_t kLcdData3 = GPIO_NUM_7;

inline constexpr int kLcdSpiHost = 2;          // SPI2_HOST
inline constexpr int kLcdPclkHz = 40 * 1000 * 1000;

/** Panel geometry. The glass is natively 368 wide and 448 tall. */
inline constexpr int kPanelWidth = 368, kPanelHeight = 448;

/** Shared I2C bus: touch, PMIC, RTC and the power-gating expander. */
inline constexpr gpio_num_t kI2cSda = GPIO_NUM_15;
inline constexpr gpio_num_t kI2cScl = GPIO_NUM_14;
inline constexpr int kI2cSpeedHz = 400 * 1000;

/** The TCA9554 expander (silk XCA9554) gates the AMOLED power rails on pins 0-2. */
inline constexpr uint8_t kExpanderAddress = 0x20;
inline constexpr uint32_t kExpanderPowerPins = 0b111; // Bits 0, 1 and 2.

/** The FT3168 capacitive touch controller signals. Reset is handled on the expander. */
inline constexpr gpio_num_t kTouchInt = GPIO_NUM_21;
inline constexpr uint8_t kTouchAddress = 0x38;

/**
 * The BOOT key on the board edge, shared with the strapping pins like on the other
 * Waveshare boards. A short press returns to the home page, a hold of 800 ms opens the
 * settings; page stepping belongs to the touch gestures.
 */
inline constexpr gpio_num_t kKeyBoot = GPIO_NUM_0;

/**
 * AXP2101 PMIC. The fuel gauge reports the battery percentage directly, so the level is
 * not a voltage proxy like on the boards that only sense a divider.
 */
inline constexpr uint8_t kPmicAddress = 0x34;

/**
 * The vendor example drives expander pins 0-2 low, waits 20 ms and drives them high before
 * the display and touch come up; the AMOLED rail needs that explicit power-on sequence.
 */
inline constexpr uint32_t kPowerSettleMs = 20;

// The ES8311 codec (MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10, PA 46) and the microSD slot
// (CMD 1, CLK 2, D0 3) stay unused in this target; the pins keep their default state.
} // namespace usage_panel::amoled
