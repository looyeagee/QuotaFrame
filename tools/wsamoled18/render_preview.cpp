// Host preview of the Waveshare AMOLED 1.8 screens. Writes one PBM per state into the
// output directory.
// Build from the repository root:
//   g++ -std=c++17 -O2 -Ifirmware/components/usage_core/include
//       -Ifirmware/components/usage_panel/include
//       -Ifirmware/targets/waveshare_amoled_18/main tools/wsamoled18/render_preview.cpp
//       firmware/targets/waveshare_amoled_18/main/screens.cpp
//       firmware/components/usage_panel/src/{canvas,cjk_font,cjk_font_data,panel_logic,screen_parts}.cpp
//       firmware/components/usage_core/src/usage_state.cpp -o wsamoled18_preview
#include "canvas.hpp"
#include "usage_panel/panel_logic.hpp"
#include "screens.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
using namespace usage_panel;
using namespace usage_panel::amoled;
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

/** Merges one now-playing publication: the song, its playhead and the lyric window. */
void apply_now_playing(View &v, const char *title, const char *artist, uint32_t position,
                       uint32_t duration, const char *const *lines, int line_count) {
    v.music_valid = true;
    snprintf(v.music.title, sizeof(v.music.title), "%s", title);
    snprintf(v.music.artist, sizeof(v.music.artist), "%s", artist);
    v.music.position_seconds = position;
    v.music.duration_seconds = duration;
    v.music.playing = true;
    v.music.current_line = 0;
    v.music.line_count = static_cast<uint8_t>(line_count);
    for (int i = 0; i < line_count && i < kMaxLyricLines; ++i)
        snprintf(v.music.lines[i], sizeof(v.music.lines[i]), "%s", lines[i]);
}

View design_view() {
    View v;
    v.link = {true, true, false, 0};
    v.clock = {true, 2026, 11, 22, 0, 13, 6, 47};
    v.battery = 90;
    snprintf(v.device_name, sizeof(v.device_name), "QF-WS-AMO18-B556");
    apply_deepseek(v, 2364, "CNY", 0);
    apply_minimax(v, 100, 4200, 74, 290000, 120);
    static const char *const kLyrics[] = {"让你在说谎的时候", "想到我会很心痛",
                                          "我已经不会难过", "没有什么好难过",
                                          "就当我的爱掉进了黑洞", "做过的梦是一阵旋涡",
                                          "没了你 才有出口", "没有你 才有我"};
    apply_now_playing(v, "证据", "杨乃文", 207, 275, kLyrics, 8);
    v.music.current_line = 0;
    return v;
}

void write(const Canvas &c, const std::string &dir, const char *name) {
    const std::string path = dir + "/" + name + ".pbm";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "P1\n%d %d\n", c.width(), c.height());
    for (int y = 0; y < c.height(); ++y) {
        for (int x = 0; x < c.width(); ++x) {
            const int n = y * Canvas::kNativeWidth + x;
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

    v = design_view();
    v.page = Page::Clock;
    render(v, "02_clock");

    v = design_view();
    v.page = Page::Minimax;
    render(v, "03_minimax");

    v = design_view();
    v.page = Page::Deepseek;
    render(v, "04_deepseek");

    v = design_view();
    v.page = Page::Music;
    render(v, "05_music");

    v = design_view();
    v.page = Page::Settings;
    v.focus = 0;
    render(v, "06_settings");

    // Landscape: rotation turns the finished image, so the layout must fit 448 x 368 too.
    v = design_view();
    v.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    v.page = Page::Clock;
    render(v, "07_clock_90");

    v = design_view();
    v.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    v.page = Page::Music;
    render(v, "08_music_90");

    v = design_view();
    v.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    v.page = Page::Home;
    render(v, "09_home_90");
    return 0;
}
