#include "board.hpp"
#include "board_config.hpp"
#include "driver/gpio.h"
#include "esp_io_expander.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <algorithm>
#include <cstdlib>
namespace usage_panel::amoled {
namespace {
constexpr char kTag[] = "wsamoled18";
constexpr uint32_t kDebounceMs = 30, kHoldMs = 800;
// A swipe has to move this far sideways and cover twice its vertical travel, so a diagonal
// flick or a scroll-like stroke does not turn the page.
constexpr int kSwipeMinPx = 60;
constexpr int kSwipeStraightness = 2;
constexpr uint64_t kSwipeMaxMs = 1200;
// A tap stays within this travel and this duration; anything slower becomes a hold.
constexpr int kTapMaxPx = 24;
constexpr uint64_t kTapMaxMs = 500;
// A hold is reported once the finger has rested inside this radius for the hold interval.
constexpr int kHoldRadiusPx = 40;
// A stroke that stays pending this long (a lift hidden behind failed reads) is dropped.
constexpr uint64_t kStrokeMaxMs = 5000;
} // namespace

bool Board::begin() {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = 0;
    bus_config.sda_io_num = kI2cSda;
    bus_config.scl_io_num = kI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus_config, &bus_) != ESP_OK) {
        ESP_LOGE(kTag, "i2c bus unavailable");
        return false;
    }

    // The display rails sit behind the expander: the vendor examples drive the three
    // power/reset pins low, wait and drive them high before the glass comes up. This has
    // to happen before the display's own software reset, which is why the board brings the
    // expander up even though the display driver is separate.
    esp_io_expander_handle_t expander = nullptr;
    if (esp_io_expander_new_i2c_tca9554(bus_, kExpanderAddress, &expander) == ESP_OK) {
        esp_io_expander_set_dir(expander, kExpanderPowerPins, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(expander, kExpanderPowerPins, 0);
        vTaskDelay(pdMS_TO_TICKS(kPowerSettleMs));
        esp_io_expander_set_level(expander, kExpanderPowerPins, 1);
        vTaskDelay(pdMS_TO_TICKS(kPowerSettleMs));
    } else {
        ESP_LOGW(kTag, "power expander missing; display rails keep their boot state");
    }

    rtc_ready_ = rtc_.begin(bus_);
    const bool pmic_ready = pmic_.begin(bus_);

    gpio_config_t keys{};
    keys.mode = GPIO_MODE_INPUT;
    keys.pull_up_en = GPIO_PULLUP_ENABLE;
    keys.pin_bit_mask = (1ULL << kKeyBoot);
    gpio_config(&keys);

    // Touch: the FT3168 speaks the FocalTech register set the FT5x06 driver implements,
    // at the same 0x38 address. Coordinates stay in the native panel space; the gesture
    // code maps them into the logical orientation itself.
    esp_lcd_panel_io_i2c_config_t touch_io_config{};
    touch_io_config.dev_addr = kTouchAddress;
    touch_io_config.scl_speed_hz = kI2cSpeedHz;
    touch_io_config.control_phase_bytes = 1;
    touch_io_config.lcd_cmd_bits = 8;
    touch_io_config.flags.disable_control_phase = 1;
    esp_lcd_panel_io_handle_t touch_io = nullptr;
    if (esp_lcd_new_panel_io_i2c(bus_, &touch_io_config, &touch_io) != ESP_OK) {
        ESP_LOGW(kTag, "touch io unavailable");
    } else {
        esp_lcd_touch_config_t touch_config{};
        touch_config.x_max = kPanelWidth;
        touch_config.y_max = kPanelHeight;
        touch_config.rst_gpio_num = GPIO_NUM_NC;
        touch_config.int_gpio_num = kTouchInt;
        touch_config.levels.reset = 0;
        touch_config.levels.interrupt = 0;
        if (esp_lcd_touch_new_i2c_ft5x06(touch_io, &touch_config, &touch_) != ESP_OK) {
            ESP_LOGW(kTag, "FT3168 not responding");
            touch_ = nullptr;
        }
    }

    ESP_LOGI(kTag, "RTC=%d PMIC=%d touch=%d", rtc_ready_, pmic_ready, touch_ != nullptr);
    return true;
}

void Board::read_battery(View &v) {
    // The AXP2101 fuel gauge reports the charge state directly, so the shown percentage is
    // a gauge reading rather than a mapped voltage.
    const int percent = pmic_.battery_percent();
    v.battery = percent;
    if (percent < 0 && pmic_.battery_voltage_mv() > 0) {
        // Connected but the gauge has not settled yet: show the voltage-derived value
        // rather than hiding the icon entirely.
        const uint16_t mv = pmic_.battery_voltage_mv();
        v.battery = static_cast<int>(
            std::clamp((mv - 3300) / (4150.f - 3300.f) * 100.f, 0.f, 100.f));
    }
}

void Board::poll(View &v) {
    LocalCalendarTime t;
    v.clock.valid = rtc_ready_ && rtc_.read(t);
    if (v.clock.valid)
        v.clock = {true, t.year, t.month, t.day, t.weekday, t.hour, t.minute, t.second};
    if (v.now_ms >= battery_at_) {
        battery_at_ = v.now_ms + 10000;
        read_battery(v);
    }
}

bool Board::sync(const LocalCalendarTime &t) { return rtc_ready_ && rtc_.write(t); }

void Board::logical_delta(int &dx, int &dy) const {
    // The canvas draws each rotation by remapping logical coordinates into the native
    // panel space; a touch delta follows the inverse map so a leftwards flick is "next
    // page" on the glass in every orientation.
    if (rotation_ == static_cast<uint8_t>(Rotation::Portrait)) {
        const int nx = dx;
        dx = dy;
        dy = -nx;
    } else if (rotation_ == static_cast<uint8_t>(Rotation::Flipped)) {
        dx = -dx;
        dy = -dy;
    }
}

Input Board::poll_touch(uint64_t now) {
    if (!touch_)
        return Input::None;
    // The FT3168 occasionally NACKs one poll (self-recovering). A failed read must not be
    // mistaken for a lift: judging the stroke on it would cut swipes short, so the stroke
    // stays pending until a successful read reports zero points.
    if (esp_lcd_touch_read_data(touch_) != ESP_OK) {
        if (gesture_.down && now - gesture_.start > kStrokeMaxMs)
            gesture_ = Gesture{};
        return Input::None;
    }
    uint16_t x[5] = {}, y[5] = {};
    uint8_t count = 0;
    const bool touched =
        esp_lcd_touch_get_coordinates(touch_, x, y, nullptr, &count, 5) && count >= 1;
    if (touched) {
        if (!gesture_.down) {
            gesture_ = {.down = true,
                        .multi = count >= 2,
                        .hold_reported = false,
                        .start_x = x[0],
                        .start_y = y[0],
                        .last_x = x[0],
                        .last_y = y[0],
                        .start = now};
            return Input::None;
        }
        if (count >= 2)
            gesture_.multi = true;
        gesture_.last_x = x[0];
        gesture_.last_y = y[0];
        const int dx = static_cast<int>(x[0]) - gesture_.start_x;
        const int dy = static_cast<int>(y[0]) - gesture_.start_y;
        if (!gesture_.hold_reported && !gesture_.multi && now - gesture_.start >= kHoldMs &&
            std::abs(dx) < kHoldRadiusPx && std::abs(dy) < kHoldRadiusPx) {
            gesture_.hold_reported = true;
            ESP_LOGI(kTag, "touch hold");
            return Input::Hold;
        }
        return Input::None;
    }
    if (!gesture_.down)
        return Input::None;
    // The finger lifted: judge the stroke once.
    const bool was_hold = gesture_.hold_reported, was_multi = gesture_.multi;
    const int dx = static_cast<int>(gesture_.last_x) - gesture_.start_x;
    const int dy = static_cast<int>(gesture_.last_y) - gesture_.start_y;
    const uint64_t duration = now - gesture_.start;
    gesture_ = Gesture{};
    if (was_hold || was_multi)
        return Input::None;
    if (duration <= kTapMaxMs && std::abs(dx) < kTapMaxPx && std::abs(dy) < kTapMaxPx) {
        ESP_LOGI(kTag, "touch tap");
        return Input::Home;
    }
    if (duration <= kSwipeMaxMs && std::abs(dx) >= kSwipeMinPx &&
        std::abs(dx) >= kSwipeStraightness * std::abs(dy)) {
        int lx = dx, ly = dy;
        logical_delta(lx, ly);
        ESP_LOGI(kTag, "swipe dx=%d dy=%d rot=%u", lx, ly, rotation_);
        return lx < 0 ? Input::Forward : Input::Back;
    }
    return Input::None;
}

Input Board::poll_key(uint64_t now) {
    const bool down = !gpio_get_level(kKeyBoot);
    if (down != boot_.raw) {
        boot_.raw = down;
        boot_.changed = now;
    }
    if (now - boot_.changed >= kDebounceMs && boot_.raw != boot_.down) {
        boot_.down = boot_.raw;
        if (boot_.down) {
            boot_.start = now;
            boot_.held = false;
        } else if (!boot_.held) {
            ESP_LOGI(kTag, "boot key");
            return Input::Home;
        }
    }
    if (boot_.down && !boot_.held && now - boot_.start >= kHoldMs) {
        boot_.held = true;
        ESP_LOGI(kTag, "boot key hold");
        return Input::Hold;
    }
    return Input::None;
}

Input Board::key(uint64_t now) {
    // The touch surface is the primary input; the BOOT key stays readable while a finger
    // rests on the glass, so the key is only consulted when the touch reports nothing.
    const Input touch_input = poll_touch(now);
    if (touch_input != Input::None)
        return touch_input;
    return poll_key(now);
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
    rotation_ = s.rotation;
    return s;
}

bool Board::save(const Settings &s) {
    rotation_ = std::min<uint8_t>(s.rotation, 2);
    nvs_handle_t n;
    if (nvs_open(kTag, NVS_READWRITE, &n) != ESP_OK)
        return false;
    const bool ok = nvs_set_u8(n, "cycle", s.cycle) == ESP_OK &&
                    nvs_set_u8(n, "alert", s.alert) == ESP_OK &&
                    nvs_set_u8(n, "seconds", s.seconds) == ESP_OK &&
                    nvs_set_u8(n, "rotation", rotation_) == ESP_OK && nvs_commit(n) == ESP_OK;
    nvs_close(n);
    return ok;
}
} // namespace usage_panel::amoled
