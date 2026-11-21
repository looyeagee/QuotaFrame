// Host preview of the RLCD 4.2 screens. Writes one PBM per state into the output directory.
// Build from the repository root:
//   g++ -std=c++17 -O2 -Ifirmware/components/usage_core/include
//       -Ifirmware/targets/waveshare_rlcd_42/main tools/rlcd42/render_preview.cpp
//       firmware/targets/waveshare_rlcd_42/main/{canvas,panel_logic,screens}.cpp
//       firmware/components/usage_core/src/usage_state.cpp -o rlcd42_preview
#include "canvas.hpp"
#include "panel_logic.hpp"
#include "screens.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
using namespace usage_panel;
using namespace usage_panel::rlcd;
namespace {
constexpr uint32_t kNow = 1790253127; // Fixed epoch for reproducible preview data.

/** Merges one MiniMax publication: the five-hour and weekly quota windows. */
void apply_minimax(View &v, uint8_t short_used, uint32_t short_reset, uint8_t week_used,
                   uint32_t week_reset, uint32_t age) {
    UsageUpdate u;
    u.provider = Provider::Minimax;
    u.state = SourceState::Ok;
    u.sampled_at = kNow - age;
    u.sent_at = kNow;
    u.short_window = {true, short_used, true, kNow + short_reset};
    u.week_window = {true, week_used, true, kNow + week_reset};
    v.model.apply(u, 0);
}

/** Merges one DeepSeek publication: the account balance. */
void apply_deepseek(View &v, uint32_t balance_cents, const char *currency, uint32_t age) {
    UsageUpdate u;
    u.provider = Provider::Deepseek;
    u.state = SourceState::Ok;
    u.sampled_at = kNow - age;
    u.sent_at = kNow;
    u.has_balance = true;
    u.balance_cents = balance_cents;
    snprintf(u.currency, sizeof(u.currency), "%s", currency);
    v.model.apply(u, 0);
}

View design_view() {
    View v;
    v.link = {true, true, false, 0};
    v.clock = {true, 2026, 11, 21, 6, 11, 24, 2};
    v.environment_valid = true;
    v.temperature = 25.2f;
    v.humidity = 15;
    v.battery = 75;
    snprintf(v.device_name, sizeof(v.device_name), "QF-WS-S3-R42-7A3F");
    // The values a live panel shows: a five-glyph balance and two quota windows.
    apply_deepseek(v, 3945, "CNY", 0);
    apply_minimax(v, 69, 13140, 57, 145152, 120);
    v.warning = {0, 1};
    return v;
}

void write(const Canvas &c, const std::string &dir, const char *name) {
    const std::string path = dir + "/" + name + ".pbm";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return;
    // The buffer is always native landscape; portrait frames are rotated back for viewing.
    const bool portrait = c.width() == Canvas::kNativeHeight;
    fprintf(f, "P1\n%d %d\n", c.width(), c.height());
    for (int y = 0; y < c.height(); ++y) {
        for (int x = 0; x < c.width(); ++x) {
            const int nx = portrait ? Canvas::kNativeWidth - 1 - y : x, ny = portrait ? x : y;
            const int n = ny * Canvas::kNativeWidth + nx;
            fputc((c.data()[n >> 3] & (0x80 >> (n & 7))) ? '1' : '0', f);
        }
        fputc('\n', f);
    }
    fclose(f);
}
} // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    static Canvas canvas;
    Screens screens(canvas);
    auto render = [&](const View &v, const char *name) {
        screens.render(v);
        write(canvas, dir, name);
    };
    View v = design_view();
    render(v, "01_home");
    v.page = Page::Minimax;
    render(v, "02_focus_minimax");
    v.page = Page::Deepseek;
    render(v, "03_focus_deepseek");

    View a = design_view();
    apply_minimax(a, 97, 2892, 74, 400000, 0);
    a.warning = {0, 2};
    Navigator nav;
    nav.update_alert(a);
    render(a, "04_alert");

    v = design_view();
    v.page = Page::Clock;
    render(v, "05_clock");
    v.page = Page::Settings;
    v.focus = 1;
    v.settings.cycle = 1;
    render(v, "06_settings");

    View off = design_view();
    off.link = {};
    UsageModel empty;
    off.model = empty;
    apply_deepseek(off, 123456, "USD", 18 * 60);
    apply_minimax(off, 42, 8045, 18, 281520, 18 * 60);
    render(off, "07_offline");

    View pair = design_view();
    pair.link = {true, false, true, 482913};
    render(pair, "08_pairing");

    View ota = design_view();
    ota.ota.stage = OtaStage::Receiving;
    snprintf(ota.ota.version, sizeof(ota.ota.version), "0.9.2");
    ota.ota.size = 1900000;
    ota.ota.offset = 1240000;
    render(ota, "09_ota");
    snprintf(ota.ota.version, sizeof(ota.ota.version), "1.2.3-abcdefghijklmnopq");
    render(ota, "09_ota_long_version");

    View p = design_view();
    p.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    render(p, "10_portrait_home");
    p.page = Page::Clock;
    render(p, "11_portrait_clock");
    p.page = Page::Settings;
    render(p, "12_portrait_settings");
    a.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    render(a, "13_portrait_alert");
    View full = design_view();
    apply_minimax(full, 100, 8045, 100, 281520, 0);
    render(full, "14_full_home");
    full.page = Page::Minimax;
    render(full, "15_full_focus");
    full.alert = 0;
    render(full, "16_full_alert");
    full.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    render(full, "17_full_portrait_alert");
    full.alert = -1;
    full.page = Page::Home;
    render(full, "18_full_portrait_home");

    View due = design_view();
    apply_minimax(due, 42, 0, 18, 0, 0);
    render(due, "19_reset_wait");
    due.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    render(due, "20_portrait_reset_wait");

    View partial = design_view();
    UsageUpdate missing;
    missing.provider = Provider::Minimax;
    missing.state = SourceState::Partial;
    missing.sampled_at = missing.sent_at = kNow + 1;
    missing.week_window = {true, 18, false, 0};
    partial.model.apply(missing, 0);
    render(partial, "21_partial_home");
    partial.page = Page::Minimax;
    render(partial, "22_partial_focus");
    partial.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    partial.page = Page::Home;
    render(partial, "23_partial_portrait_home");

    // The clock page offline: the OFFLINE chip shares the top row with the date and the
    // room readings in landscape, and moves to the bottom of the portrait page.
    View coff = design_view();
    coff.page = Page::Clock;
    coff.link = {};
    render(coff, "24_clock_offline");
    coff.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    render(coff, "25_clock_offline_portrait");

    // Long room readings: a three-digit humidity and a negative temperature.
    View wide = design_view();
    wide.page = Page::Clock;
    wide.humidity = 100;
    wide.temperature = -12.3f;
    render(wide, "26_clock_wide_readings");
    wide.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    render(wide, "27_clock_wide_readings_portrait");

    // No sensor reading: the room values fall back to dashes.
    View nos = design_view();
    nos.page = Page::Clock;
    nos.environment_valid = false;
    render(nos, "28_clock_no_readings");

    // An unknown battery level: the meter shows a placeholder at the usual position.
    View nobat = design_view();
    nobat.battery = -1;
    render(nobat, "29_home_no_battery");
    return 0;
}
