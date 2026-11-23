// Host preview of the Waveshare AMOLED 1.8 screens. Writes one PPM per state into the
// output directory, in panel-native order, so the file shows what the glass shows.
// Build from the repository root (the target's canvas.cpp needs ESP-IDF, so the host uses
// its own stand-in):
//   g++ -std=c++17 -O2 -Ifirmware/components/usage_core/include
//       -Ifirmware/components/usage_panel/include
//       -Ifirmware/targets/waveshare_amoled_18/main tools/wsamoled18/render_preview.cpp
//       tools/wsamoled18/host_canvas.cpp
//       firmware/targets/waveshare_amoled_18/main/screens.cpp
//       firmware/components/usage_panel/src/{canvas,cjk_font,cjk_font_data,panel_logic,screen_parts}.cpp
//       firmware/components/usage_core/src/usage_state.cpp -o wsamoled18_preview
#include "canvas.hpp"
#include "usage_panel/panel_logic.hpp"
#include "screens.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
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

/** One live session of the preview roster. */
struct SessionSample {
    const char *name;
    SessionState state;
    WaitingReason reason;
};

/** Fills the live session list the sessions page draws. */
void apply_sessions(View &v, std::initializer_list<SessionSample> live) {
    v.sessions = {};
    v.sessions_valid = true;
    v.sessions.live_count = static_cast<uint8_t>(live.size());
    int i = 0;
    for (const auto &s : live) {
        snprintf(v.sessions.live[i].name, sizeof(v.sessions.live[i].name), "%s", s.name);
        v.sessions.live[i].state = s.state;
        v.sessions.live[i].waiting_for = s.reason;
        ++i;
    }
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

/**
 * Writes one frame as a PPM.
 *
 * The palette is held in the panel's byte order, so each pixel is turned back into RGB565
 * before it is expanded to three bytes; rows go out in native order, which is the order the
 * glass receives them in.
 */
void write(const Canvas &c, const std::string &dir, const char *name) {
    const std::string path = dir + "/" + name + ".ppm";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", Canvas::kNativeWidth, Canvas::kNativeHeight);
    const uint16_t *frame = c.color_data();
    for (int y = 0; y < Canvas::kNativeHeight; ++y) {
        for (int x = 0; x < Canvas::kNativeWidth; ++x) {
            const uint16_t wire = frame[y * Canvas::kNativeWidth + x];
            const uint16_t v = static_cast<uint16_t>((wire << 8) | (wire >> 8));
            fputc(((v >> 11) & 0x1f) * 255 / 31, f);
            fputc(((v >> 5) & 0x3f) * 255 / 63, f);
            fputc((v & 0x1f) * 255 / 31, f);
        }
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

    // The Bridge can turn the glass off; the brightness row shows that step, not the dimmest
    // lit one, so the row and the glass agree.
    v = design_view();
    v.page = Page::Settings;
    v.focus = 4;
    v.settings.brightness = 0;
    render(v, "06_settings_off");

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

    // Sessions: what is running now, and nothing else. The count carries the state colour,
    // each card carries it a second time as a bar and a word.
    v = design_view();
    v.page = Page::Sessions;
    apply_sessions(v, {{"问候", SessionState::Idle, WaitingReason::None}});
    render(v, "10_sessions_one");

    v = design_view();
    v.page = Page::Sessions;
    apply_sessions(v, {{"重构会话页", SessionState::Busy, WaitingReason::None},
                       {"QuotaFrame bridge", SessionState::Busy, WaitingReason::None}});
    render(v, "11_sessions_working");

    v = design_view();
    v.page = Page::Sessions;
    apply_sessions(v,
                   {{"重构会话页", SessionState::Busy, WaitingReason::None},
                    {"修 BLE 重连", SessionState::Waiting, WaitingReason::Ask},
                    {"QuotaFrame bridge", SessionState::Busy, WaitingReason::None},
                    {"整理发布文档", SessionState::Idle, WaitingReason::None}});
    render(v, "12_sessions_four");

    v = design_view();
    v.page = Page::Sessions;
    apply_sessions(v, {});
    render(v, "13_sessions_empty");

    v = design_view();
    v.page = Page::Sessions;
    apply_sessions(v, {{"提问功能选项测试", SessionState::Waiting, WaitingReason::Ask},
                       {"等待授权", SessionState::Waiting, WaitingReason::Permission}});
    render(v, "14_sessions_waiting");

    // A roster longer than the glass: the page shows a window of it, and the
    // touch surface scrolls through the rest.
    v = design_view();
    v.page = Page::Sessions;
    apply_sessions(v, {{"重构会话页", SessionState::Busy, WaitingReason::None},
                       {"修 BLE 重连", SessionState::Waiting, WaitingReason::Ask},
                       {"QuotaFrame bridge", SessionState::Busy, WaitingReason::None},
                       {"整理发布文档", SessionState::Idle, WaitingReason::None},
                       {"面板滚动", SessionState::Busy, WaitingReason::None},
                       {"整理 bridge 日志", SessionState::Idle, WaitingReason::None},
                       {"会话页改版", SessionState::Busy, WaitingReason::None},
                       {"ota 验证", SessionState::Waiting, WaitingReason::Permission},
                       {"固件发布", SessionState::Idle, WaitingReason::None}});
    render(v, "15_sessions_many");

    v = design_view();
    v.page = Page::Sessions;
    apply_sessions(v, {{"重构会话页", SessionState::Busy, WaitingReason::None},
                       {"修 BLE 重连", SessionState::Waiting, WaitingReason::Ask},
                       {"QuotaFrame bridge", SessionState::Busy, WaitingReason::None},
                       {"整理发布文档", SessionState::Idle, WaitingReason::None},
                       {"面板滚动", SessionState::Busy, WaitingReason::None},
                       {"整理 bridge 日志", SessionState::Idle, WaitingReason::None},
                       {"会话页改版", SessionState::Busy, WaitingReason::None},
                       {"ota 验证", SessionState::Waiting, WaitingReason::Permission},
                       {"固件发布", SessionState::Idle, WaitingReason::None}});
    v.sessions_geometry = {Screens::kSessionsPitch, Screens::kSessionsRowsMax};
    v.sessions_scroll_px = 5 * Screens::kSessionsPitch;
    render(v, "16_sessions_scrolled");

    // The list between two rows: the offset counts pixels, so a drag that stopped halfway
    // through a row leaves the card half under the count.
    v = design_view();
    v.page = Page::Sessions;
    apply_sessions(v, {{"重构会话页", SessionState::Busy, WaitingReason::None},
                       {"修 BLE 重连", SessionState::Waiting, WaitingReason::Ask},
                       {"QuotaFrame bridge", SessionState::Busy, WaitingReason::None},
                       {"整理发布文档", SessionState::Idle, WaitingReason::None},
                       {"面板滚动", SessionState::Busy, WaitingReason::None},
                       {"整理 bridge 日志", SessionState::Idle, WaitingReason::None},
                       {"会话页改版", SessionState::Busy, WaitingReason::None},
                       {"ota 验证", SessionState::Waiting, WaitingReason::Permission},
                       {"固件发布", SessionState::Idle, WaitingReason::None}});
    v.sessions_geometry = {Screens::kSessionsPitch, Screens::kSessionsRowsMax};
    v.sessions_scroll_px = 4 * Screens::kSessionsPitch + 37;
    render(v, "17_sessions_half_row");
    return 0;
}
