#include "axp2101.hpp"
#include "board_config.hpp"
#include "esp_log.h"
namespace usage_panel::amoled {
namespace {
constexpr char kTag[] = "axp2101";
// Register map of the AXP2101, as decoded by XPowersLib for the same chip.
constexpr uint8_t kRegStatus1 = 0x00;     // Bit 3 battery connected, bit 5 VBUS good.
constexpr uint8_t kRegStatus2 = 0x01;     // Bit 3 clear means VBUS inserted.
constexpr uint8_t kRegIcType = 0x03;      // Reads 0x4A on the AXP2101.
constexpr uint8_t kRegAdcChannelCtrl = 0x30;
constexpr uint8_t kRegAdcBatVoltH = 0x34; // Battery voltage, 5 high bits then 8 low, 1 mV/LSB.
constexpr uint8_t kRegAdcBatVoltL = 0x35;
constexpr uint8_t kRegBatDetCtrl = 0x68;  // Bit 0 enables battery detection.
constexpr uint8_t kRegBatPercent = 0xA4;  // Fuel gauge charge in percent.
constexpr uint8_t kAxp2101Id = 0x4A;
constexpr uint8_t kBitBatteryConnected = 1 << 3;
constexpr uint8_t kBitVbusGood = 1 << 5;
constexpr uint8_t kBitVbusNotInserted = 1 << 3;
} // namespace

bool Axp2101::begin(i2c_master_bus_handle_t bus) {
    // Named field by field: this build compiles with -Werror and the config gained fields
    // the bare designated initializer would leave unnamed.
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kPmicAddress;
    config.scl_speed_hz = kI2cSpeedHz;
    if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK) {
        device_ = nullptr;
        return false;
    }
    uint8_t id = 0;
    if (!read_u8(kRegIcType, id) || id != kAxp2101Id) {
        ESP_LOGW(kTag, "unexpected IC type 0x%02x", id);
        return false;
    }
    // Battery detection feeds the connection status bit; the voltage ADC channel 0 feeds
    // the voltage registers. Both stay off after reset until they are asked for.
    uint8_t det = 0, adc = 0;
    if (read_u8(kRegBatDetCtrl, det) && !(det & 0x01))
        write_u8(kRegBatDetCtrl, det | 0x01);
    if (read_u8(kRegAdcChannelCtrl, adc) && !(adc & 0x01))
        write_u8(kRegAdcChannelCtrl, adc | 0x01);
    return true;
}

int Axp2101::battery_percent() {
    uint8_t status = 0, percent = 0;
    if (!read_u8(kRegStatus1, status) || !(status & kBitBatteryConnected))
        return -1;
    if (!read_u8(kRegBatPercent, percent))
        return -1;
    // The gauge saturates below 0 and above 100 only when misconfigured; clamp anyway.
    return percent > 100 ? 100 : percent;
}

uint16_t Axp2101::battery_voltage_mv() {
    uint8_t status = 0, high = 0, low = 0;
    if (!read_u8(kRegStatus1, status) || !(status & kBitBatteryConnected))
        return 0;
    if (!read(kRegAdcBatVoltH, &high, 1) || !read(kRegAdcBatVoltL, &low, 1))
        return 0;
    return static_cast<uint16_t>(((high & 0x1F) << 8) | low);
}

bool Axp2101::vbus_present() {
    uint8_t status1 = 0, status2 = 0;
    if (!read_u8(kRegStatus1, status1) || !read_u8(kRegStatus2, status2))
        return false;
    return (status1 & kBitVbusGood) != 0 && (status2 & kBitVbusNotInserted) == 0;
}

bool Axp2101::read(uint8_t reg, uint8_t *data, size_t length) {
    return device_ != nullptr &&
           i2c_master_transmit_receive(device_, &reg, 1, data, length, 100) == ESP_OK;
}

bool Axp2101::read_u8(uint8_t reg, uint8_t &value) { return read(reg, &value, 1); }

bool Axp2101::write_u8(uint8_t reg, uint8_t value) {
    const uint8_t buffer[2] = {reg, value};
    return device_ != nullptr && i2c_master_transmit(device_, buffer, 2, 100) == ESP_OK;
}
} // namespace usage_panel::amoled
