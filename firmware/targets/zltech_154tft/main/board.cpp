#include "board.hpp"
#include "board_config.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include <algorithm>
#include <cstdio>
namespace usage_panel::zltech {
namespace {
constexpr char kTag[] = "zltech154";
constexpr uint32_t kDebounceMs = 30, kHoldMs = 800;
// Linear voltage estimate; readings below kNoBatteryVolts report an unknown battery level.
// A 3.7 V cell sits near the middle of this range for most of its discharge, so the level is
// a voltage proxy rather than a charge measurement.
constexpr float kEmptyVolts = 3.3f, kFullVolts = 4.15f, kNoBatteryVolts = 2.8f;

/** Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm). */
int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/** Civil date for a day count since 1970-01-01. */
void civil_from_days(int64_t z, int &y, unsigned &m, unsigned &d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yr = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yr + (m <= 2));
}
} // namespace

bool Board::begin() {
    gpio_config_t keys{};
    keys.mode = GPIO_MODE_INPUT;
    keys.pull_up_en = GPIO_PULLUP_ENABLE;
    keys.pin_bit_mask =
        (1ULL << kKeyBack) | (1ULL << kKeyHome) | (1ULL << kKeyForward);
    if (gpio_config(&keys) != ESP_OK)
        return false;

    adc_oneshot_unit_init_cfg_t unit{};
    unit.unit_id = ADC_UNIT_2;
    if (adc_oneshot_new_unit(&unit, &adc_) == ESP_OK) {
        adc_oneshot_chan_cfg_t channel{};
        channel.atten = ADC_ATTEN_DB_12;
        channel.bitwidth = ADC_BITWIDTH_12;
        adc_oneshot_config_channel(adc_, static_cast<adc_channel_t>(kBatteryChannel), &channel);
        adc_cali_curve_fitting_config_t cali{};
        cali.unit_id = ADC_UNIT_2;
        cali.chan = static_cast<adc_channel_t>(kBatteryChannel);
        cali.atten = ADC_ATTEN_DB_12;
        cali.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cali, &cali_) != ESP_OK)
            cali_ = nullptr;
    }
    ESP_LOGI(kTag, "ADC=%d", cali_ != nullptr);
    return true;
}

void Board::read_battery(View &v) {
    int raw = 0, millivolts = 0;
    if (!adc_ || !cali_ || adc_oneshot_read(adc_, static_cast<adc_channel_t>(kBatteryChannel),
                                            &raw) != ESP_OK ||
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
    if (clock_valid_) {
        const int64_t now = clock_epoch_ + static_cast<int64_t>((v.now_ms - clock_at_ms_) / 1000);
        const int64_t days = now / 86400;
        int64_t second_of_day = now % 86400;
        if (second_of_day < 0) {
            second_of_day += 86400;
        }
        int year = 0;
        unsigned month = 0, day = 0;
        civil_from_days(days, year, month, day);
        const int weekday = static_cast<int>(((days % 7) + 11) % 7); // 1970-01-01 is Thursday.
        v.clock = {true, static_cast<uint16_t>(year), static_cast<uint8_t>(month),
                   static_cast<uint8_t>(day), static_cast<uint8_t>(weekday),
                   static_cast<uint8_t>(second_of_day / 3600),
                   static_cast<uint8_t>((second_of_day / 60) % 60),
                   static_cast<uint8_t>(second_of_day % 60)};
    } else {
        v.clock = ClockTime{};
    }
    if (v.now_ms >= battery_at_) {
        battery_at_ = v.now_ms + 10000;
        read_battery(v);
    }
}

bool Board::sync(const LocalCalendarTime &t) {
    // The board has no RTC, so the sync only anchors the RAM clock. Reject dates the protocol
    // could not have produced so a corrupt field cannot become a plausible-looking time.
    if (t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 ||
        t.hour > 23 || t.minute > 59 || t.second > 59)
        return false;
    // The publication carries the host's local calendar fields, so the panel keeps local wall
    // time and the UTC offset is only informational. Anchoring on UTC here made the clock read
    // the host's UTC hour, which is off by the zone for most of the day.
    const int64_t days = days_from_civil(t.year, t.month, t.day);
    clock_epoch_ = days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    clock_at_ms_ = esp_timer_get_time() / 1000;
    clock_valid_ = true;
    return true;
}

Input Board::key(uint64_t now) {
    struct Key {
        gpio_num_t pin;
        Input action;
        bool raw = false, down = false, held = false;
        uint64_t changed = 0, start = 0;
    };
    // The keys of the top edge, from left to right.
    static Key keys[] = {{kKeyBack, Input::Back},
                         {kKeyHome, Input::Home},
                         {kKeyForward, Input::Forward}};
    for (auto &k : keys) {
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
            } else if (!k.held) {
                ESP_LOGI(kTag, "key gpio=%d", static_cast<int>(k.pin));
                return k.action;
            }
        }
        if (k.down && !k.held && now - k.start >= kHoldMs) {
            k.held = true;
            ESP_LOGI(kTag, "key gpio=%d hold", static_cast<int>(k.pin));
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

} // namespace usage_panel::zltech
