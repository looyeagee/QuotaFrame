#pragma once
#include "driver/i2c_master.h"
#include <cstdint>
namespace usage_panel::amoled {
/**
 * Minimal AXP2101 PMIC driver for battery status.
 *
 * The PMIC also gates the display rails in hardware, but the V1 board brings the panel up
 * through the TCA9554 expander instead, so this driver only enables the fuel gauge and
 * reads it: the on-chip gauge reports the charge state directly, which keeps the shown
 * percentage from being a voltage proxy.
 */
class Axp2101 {
  public:
    /** Adds the device at 0x34 and enables battery detection and the voltage ADC. */
    bool begin(i2c_master_bus_handle_t bus);
    /** Battery charge in [0, 100], or -1 when no battery is connected or the PMIC is absent. */
    int battery_percent();
    /** Battery voltage in millivolts, or 0 when no battery is connected. */
    uint16_t battery_voltage_mv();
    /** True when a supply feeds the board through the USB port. */
    bool vbus_present();

  private:
    bool read(uint8_t reg, uint8_t *data, size_t length);
    bool read_u8(uint8_t reg, uint8_t &value);
    bool write_u8(uint8_t reg, uint8_t value);
    i2c_master_dev_handle_t device_ = nullptr;
};
} // namespace usage_panel::amoled
