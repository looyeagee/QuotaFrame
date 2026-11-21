#include "board.hpp"
#include "panel_logic.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace usage_panel::rlcd {
namespace {
constexpr char kTag[] = "rlcd42";
constexpr gpio_num_t kSda = GPIO_NUM_13, kScl = GPIO_NUM_14;
// KEY sits left of the case and BOOT right of it; the PWR button between them switches the
// power circuit and reaches no GPIO, so its action comes from the chord of both keys.
constexpr gpio_num_t kKeyLeft = GPIO_NUM_18, kKeyRight = GPIO_NUM_0;
constexpr uint8_t kShtc3Address = 0x70;
constexpr uint32_t kDebounceMs = 30, kHoldMs = 800, kChordMs = 100;
// The battery is sensed on GPIO4 (ADC1 channel 3) through a 1:3 divider.
constexpr adc_channel_t kBatteryChannel = ADC_CHANNEL_3;
constexpr float kBatteryDivider = 3.f;
// Linear voltage estimate; readings below kNoBatteryVolts report an unknown battery level.
constexpr float kEmptyVolts = 3.0f, kFullVolts = 4.12f, kNoBatteryVolts = 2.5f;
uint8_t crc8(const uint8_t *data) {
    uint8_t c = 0xff;
    for (int i = 0; i < 2; ++i) {
        c ^= data[i];
        for (int j = 0; j < 8; ++j)
            c = (c & 0x80) ? (c << 1) ^ 0x31 : c << 1;
    }
    return c;
}
} // namespace

bool Board::begin() {
    i2c_master_bus_config_t c{};
    c.i2c_port = I2C_NUM_0;
    c.sda_io_num = kSda;
    c.scl_io_num = kScl;
    c.clk_source = I2C_CLK_SRC_DEFAULT;
    c.glitch_ignore_cnt = 7;
    c.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&c, &bus_) != ESP_OK)
        return false;
    rtc_ready_ = rtc_.begin(bus_);
    i2c_device_config_t d{};
    d.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    d.device_address = kShtc3Address;
    d.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_, &d, &sht_));

    gpio_config_t keys{};
    keys.mode = GPIO_MODE_INPUT;
    keys.pull_up_en = GPIO_PULLUP_ENABLE;
    keys.pin_bit_mask = (1ULL << kKeyLeft) | (1ULL << kKeyRight);
    ESP_ERROR_CHECK(gpio_config(&keys));

    adc_oneshot_unit_init_cfg_t unit{};
    unit.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit, &adc_) == ESP_OK) {
        adc_oneshot_chan_cfg_t channel{};
        channel.atten = ADC_ATTEN_DB_12;
        channel.bitwidth = ADC_BITWIDTH_12;
        adc_oneshot_config_channel(adc_, kBatteryChannel, &channel);
        adc_cali_curve_fitting_config_t cali{};
        cali.unit_id = ADC_UNIT_1;
        cali.chan = kBatteryChannel;
        cali.atten = ADC_ATTEN_DB_12;
        cali.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cali, &cali_) != ESP_OK)
            cali_ = nullptr;
    }

    ESP_LOGI(kTag, "RTC=%d ADC=%d", rtc_ready_, cali_ != nullptr);
    return true;
}

void Board::read_environment(View &v) {
    const uint8_t wake[] = {0x35, 0x17}, measure[] = {0x78, 0x66}, sleep[] = {0xb0, 0x98};
    uint8_t data[6]{};
    bool ok = i2c_master_transmit(sht_, wake, 2, 100) == ESP_OK;
    // The SHTC3 requires up to 240 us to wake; one RTOS tick has no minimum elapsed duration.
    esp_rom_delay_us(300);
    ok = ok && i2c_master_transmit(sht_, measure, 2, 100) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(15));
    ok = ok && i2c_master_receive(sht_, data, 6, 100) == ESP_OK;
    i2c_master_transmit(sht_, sleep, 2, 100);
    v.environment_valid = ok && crc8(data) == data[2] && crc8(data + 3) == data[5];
    if (v.environment_valid) {
        v.temperature = -45 + 175.f * ((data[0] << 8) | data[1]) / 65536.f;
        v.humidity = 100.f * ((data[3] << 8) | data[4]) / 65536.f;
    }
}

void Board::read_battery(View &v) {
    int raw = 0, millivolts = 0;
    if (!adc_ || !cali_ || adc_oneshot_read(adc_, kBatteryChannel, &raw) != ESP_OK ||
        adc_cali_raw_to_voltage(cali_, raw, &millivolts) != ESP_OK) {
        v.battery = -1;
        return;
    }
    const float volts = millivolts * kBatteryDivider / 1000.f;
    if (volts < kNoBatteryVolts) {
        v.battery = -1;
        return;
    }
    v.battery = static_cast<int>(
        std::clamp((volts - kEmptyVolts) / (kFullVolts - kEmptyVolts) * 100.f, 0.f, 100.f));
}

void Board::poll(View &v) {
    LocalCalendarTime t;
    v.clock.valid = rtc_ready_ && rtc_.read(t);
    if (v.clock.valid)
        v.clock = {true, t.year, t.month, t.day, t.weekday, t.hour, t.minute, t.second};
    if (v.now_ms >= environment_at_) {
        environment_at_ = v.now_ms + 60000;
        read_environment(v);
    }
    if (v.now_ms >= battery_at_) {
        battery_at_ = v.now_ms + 10000;
        read_battery(v);
    }
}

bool Board::sync(const LocalCalendarTime &t) { return rtc_ready_ && rtc_.write(t); }

Input Board::key(uint64_t now) {
    struct Key {
        gpio_num_t pin;
        bool raw = false, down = false, held = false, consumed = false;
        uint64_t changed = 0, start = 0;
    };
    static Key keys[] = {{kKeyLeft}, {kKeyRight}};
    for (int i = 0; i < 2; ++i) {
        auto &k = keys[i];
        const bool down = !gpio_get_level(k.pin);
        if (down != k.raw) {
            k.raw = down;
            k.changed = now;
        }
        if (now - k.changed >= kDebounceMs && k.raw != k.down) {
            k.down = k.raw;
            if (k.down) {
                k.start = now;
                k.held = false;
                k.consumed = false;
                // Both keys pressed together stand in for the middle button, which reaches no
                // GPIO; the chord swallows the two presses so neither key steps a page.
                auto &other = keys[1 - i];
                if (other.down && now - other.start <= kChordMs) {
                    k.consumed = true;
                    other.consumed = true;
                    ESP_LOGI(kTag, "chord");
                    return Input::Home;
                }
            } else if (!k.held && !k.consumed) {
                ESP_LOGI(kTag, "key %s", i == 0 ? "left" : "right");
                return i == 0 ? Input::Back : Input::Forward;
            }
        }
        if (k.down && !k.held && !k.consumed && now - k.start >= kHoldMs) {
            k.held = true;
            ESP_LOGI(kTag, "hold %s", i == 0 ? "left" : "right");
            return Input::Hold;
        }
    }
    return Input::None;
}

Settings Board::load() {
    Settings s;
    nvs_handle_t n;
    if (nvs_open(kTag, NVS_READONLY, &n) == ESP_OK) {
        nvs_get_u8(n, "cycle", &s.cycle);
        nvs_get_u8(n, "alert", &s.alert);
        nvs_get_u8(n, "seconds", &s.seconds);
        nvs_get_u8(n, "rotation", &s.rotation);
        nvs_close(n);
    }
    s.cycle = std::min<uint8_t>(s.cycle, 3);
    s.alert = std::min<uint8_t>(s.alert, 2);
    s.seconds = s.seconds ? 1 : 0;
    s.rotation = std::min<uint8_t>(s.rotation, 2);
    return s;
}

bool Board::save(const Settings &s) {
    nvs_handle_t n;
    if (nvs_open(kTag, NVS_READWRITE, &n) != ESP_OK)
        return false;
    const bool ok = nvs_set_u8(n, "cycle", s.cycle) == ESP_OK &&
                    nvs_set_u8(n, "alert", s.alert) == ESP_OK &&
                    nvs_set_u8(n, "seconds", s.seconds) == ESP_OK &&
                    nvs_set_u8(n, "rotation", s.rotation) == ESP_OK && nvs_commit(n) == ESP_OK;
    nvs_close(n);
    return ok;
}

} // namespace usage_panel::rlcd
