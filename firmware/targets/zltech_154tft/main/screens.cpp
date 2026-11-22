#include "screens.hpp"
#include "usage_panel/cjk_font.hpp"
#include "usage_panel/panel_logic.hpp"
#include "usage_panel/screen_parts.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace usage_panel::zltech {
namespace {
// Content sits 6 px inside the 2 px bezel frame drawn 4 px from the panel edge.
constexpr int kLeft = 10, kBezel = 4;
// Pixel-font line height at the label scale.
constexpr int kLine = 14;
// Pixel-font line height at the small scale.
constexpr int kSmallLine = kLine / 2;
// Inverted chips extend this far around their text, so the text aligns with plain labels.
constexpr int kChipPad = 4;
// First content line, just below the header rule.
constexpr int kContentTop = 32;
// Smallest pace excess, in percentage points, that the detail page reports as OVER. Smaller
// deltas are within the rounding of whole-percent usage and the fixed window lengths.
constexpr int kOverThreshold = 5;
// Footer rule on pages with a footer line; the footer text follows 6 px below it.
constexpr int kFooterRule = 206, kFooterText = 212;
// Music page: two lyric-font rows for the song, a playhead, then the lyric window.
constexpr int kMusicScale = 2;
constexpr int kMusicLineHeight = CjkText::line_height(kMusicScale); // 24 px
constexpr int kMusicTitleRow = 30, kMusicArtistRow = 56;
constexpr int kMusicBarTop = 80, kMusicBarHeight = 12;
constexpr int kMusicRule = 96;
// The lyric window: one row per line, so a row never reaches into the row below it.
constexpr int kMusicLyricsTop = 100, kMusicLyricPitch = 24, kMusicLyricRows = 5;
// The inverted lyric row extends this far past the text margin, like the status chips.
constexpr int kMusicTextInset = 4;

// Segment heights the balance figure tries, largest first; the smallest one is the
// fallback when none of them fits.
constexpr int kBalanceHeights[] = {72, 64, 56, 48, 44, 40, 36, 32, 28, 24};
} // namespace

int Screens::battery(const View &v, int right, Ink ink) {
    char pct[8];
    if (v.battery < 0)
        snprintf(pct, sizeof(pct), "--%%");
    else
        snprintf(pct, sizeof(pct), "%d%%", std::clamp(v.battery, 0, 100));
    const int tw = Canvas::text_width(pct, 2);
    const int x = right - tw;
    c_.text(x, 8, pct, 2, ink);
    return x;
}

int Screens::chip(int x, int top, const char *s, Ink ink) {
    const Ink paper = ink == Ink::Black ? Ink::White : Ink::Black;
    const int tw = Canvas::text_width(s, 2);
    const int x0 = x - kChipPad, y0 = top - kChipPad;
    const int w = tw + 2 * kChipPad, h = kLine + 2 * kChipPad;
    c_.fill(x0, y0, w, h, ink);
    // A 2 px corner radius: clear the three outermost pixels of each corner.
    for (int cx : {x0, x0 + w - 1})
        for (int cy : {y0, y0 + h - 1}) {
            const int dx = cx == x0 ? 1 : -1, dy = cy == y0 ? 1 : -1;
            c_.pixel(cx, cy, paper);
            c_.pixel(cx + dx, cy, paper);
            c_.pixel(cx, cy + dy, paper);
        }
    c_.text(x, top, s, 2, paper);
    return tw;
}

void Screens::header(const View &v, const char *title) {
    char time[8];
    snprintf(time, sizeof(time), "%02u:%02u", v.clock.hour, v.clock.minute);
    c_.text(kLeft, 8, title ? title : v.clock.valid ? time : "--:--", 2);
    int edge = battery(v, c_.width() - kLeft);
    const char *status = link_status(v);
    // LINKED is the normal state and is not shown; every other state gets a chip.
    if (v.link.has_passkey || v.ota.stage != OtaStage::Idle || !v.link.encrypted) {
        const int tw = Canvas::text_width(status, 2);
        edge -= 8 + kChipPad + tw;
        chip(edge, 8, status);
    }
    c_.fill(kLeft, 26, c_.width() - 2 * kLeft, 2);
}

void Screens::page_dots(int page) {
    const int pages = ring_pages();
    const int total = pages * 12 - 4;
    int x = (c_.width() - total) / 2;
    for (int i = 0; i < pages; ++i) {
        if (i == page)
            c_.fill(x + 12 * i, 222, 8, 8);
        else
            c_.frame(x + 12 * i, 222, 8, 8, 2);
    }
}

void Screens::bar(int x, int y, int w, int h, int percent, float marker) {
    constexpr int kInset = 3;
    c_.frame(x, y, w, h, 2);
    const int cx = x + kInset, cw = w - 2 * kInset;
    if (percent > 0) {
        const int lit = std::min(round_px(std::min(percent, 100) * cw / 100.f), cw);
        c_.fill(cx, y + kInset, lit, h - 2 * kInset);
    }
    if (marker < 0)
        return;
    // Pace marker: a tick inside the bar, separated from the fill by a white gap on both
    // sides of it, so it stays readable over the filled part. The gap stays inside the frame,
    // which keeps the marker from eating the border at the ends of the bar.
    const int mx = std::clamp(cx + round_px(cw * marker), cx + 3, cx + cw - 3);
    c_.fill(mx - 3, y + 2, 6, h - 4, Ink::White);
    c_.fill(mx - 1, y + 2, 2, h - 4);
}

int Screens::big_percent_width(int h) {
    const Digits d = digits_for(h);
    return Canvas::segments_width("h88", d.w, d.t) + percent_gap(h) + percent_width(h);
}

int Screens::big_percent(int x, int top, int percent, int h, Ink ink) {
    const Digits d = digits_for(h);
    char b[8];
    percent_digits(percent, b, sizeof(b));
    // The percent sign stays at a fixed position after the field, including for missing values.
    const int w = c_.segments(x, top, b, d.w, h, d.t, ink);
    percent_sign(c_, x + w + percent_gap(h), top + h, h, ink);
    return w + percent_gap(h) + percent_width(h);
}

void Screens::home(const View &v) {
    header(v, nullptr);
    const int right = c_.width() - kLeft;
    char b[32];
    const auto &d = v.model.snapshot(provider_for(0));
    const auto &m = v.model.snapshot(provider_for(1));

    // DeepSeek block: the caption row and the figure underneath it.
    c_.text(kLeft, kContentTop, kProviderNames[0], 2);
    if (balance_present(d, v.link.encrypted)) {
        const int h = balance_height(d, right - kLeft, 40, kBalanceHeights);
        draw_balance(c_, kLeft, 50, d, right - kLeft, h, kBalanceHeights);
    } else {
        c_.text(kLeft, 52, empty_message(v), 2);
    }
    if (age_label(v, 0, b, sizeof(b)))
        c_.text_right(right, 50 + 18, b, 1);
    c_.dotted(kLeft, 96, right - kLeft);

    // MiniMax block: both windows as a label row, a bar under it, then the weekly window.
    c_.text(kLeft, 102, kProviderNames[1], 2);
    const bool sp = short_present(m, v.link.encrypted), wp = week_present(m, v.link.encrypted);
    if (!sp && !wp) {
        c_.text(kLeft, 124, empty_message(v), 2);
        return;
    }
    const Paces pace = paces(v, 1, sp, wp);
    big_percent(kLeft, 120, sp ? m.short_window.used_percent : -1, 42);
    c_.text(kLeft, 164, "5H", 2);
    reset_countdown(v, 1, false, b, sizeof(b));
    c_.text_right(right, 164, b, 2);
    bar(kLeft, 178, right - kLeft, 12, sp ? m.short_window.used_percent : -1,
        marker(pace.short_pace));
    if (wp)
        snprintf(b, sizeof(b), "WEEK %u%%", m.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "WEEK --");
    c_.text(kLeft, 194, b, 2);
    reset_countdown(v, 1, true, b, sizeof(b));
    c_.text_right(right, 194, b, 2);
    bar(kLeft, 210, right - kLeft, 10, wp ? m.week_window.used_percent : -1,
        marker(pace.week_pace));
}

void Screens::balance_page(const View &v) {
    const auto &s = v.model.snapshot(provider_for(0));
    const int right = c_.width() - kLeft;
    char b[32];
    header(v, kProviderNames[0]);
    c_.text(kLeft, kContentTop, "BALANCE", 2);
    if (!balance_present(s, v.link.encrypted)) {
        c_.text(kLeft, 74, empty_message(v), 2);
        return;
    }
    // The figure sits under its caption and ends well above the footer.
    const int h = balance_height(s, right - kLeft, 88, kBalanceHeights);
    draw_balance(c_, kLeft, 52, s, right - kLeft, h, kBalanceHeights);
    if (age_label(v, 0, b, sizeof(b)))
        c_.text(kLeft, 156, b, 2);
    c_.dotted(kLeft, 172, right - kLeft);
    // The two lines keep a gap between them and stay clear of the page dots.
    c_.text(kLeft, 182, "CREDIT LEFT", 2);
    c_.text(kLeft, 202, "ON THE ACCOUNT", 2);
}

void Screens::windows_page(const View &v) {
    const auto &s = v.model.snapshot(provider_for(1));
    const int right = c_.width() - kLeft;
    char b[32];
    header(v, kProviderNames[1]);
    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    if (!sp && !wp) {
        c_.text(kLeft, kContentTop, empty_message(v), 2);
        return;
    }
    const Paces pace = paces(v, 1, sp, wp);

    // Five-hour window: the figure, the pace chip under it, then the bar and the elapsed time.
    c_.text(kLeft, kContentTop, "5H", 2);
    reset_countdown(v, 1, false, b, sizeof(b));
    c_.text_right(right, kContentTop, b, 2);
    big_percent(kLeft, 48, sp ? s.short_window.used_percent : -1, 56);
    if (pace.short_pace.valid && pace.short_pace.delta >= kOverThreshold) {
        // The chip keeps a gap to the figure, so it never merges with the digits.
        snprintf(b, sizeof(b), "OVER +%d%%", pace.short_pace.delta);
        chip(kLeft, 112, b);
    }
    bar(kLeft, 136, right - kLeft, 16, sp ? s.short_window.used_percent : -1,
        marker(pace.short_pace));
    if (pace.short_pace.valid) {
        char elapsed[16];
        format_elapsed(pace.short_pace.elapsed_s, elapsed, sizeof(elapsed));
        snprintf(b, sizeof(b), "ELAPSED %s", elapsed);
        c_.text(kLeft, 158, b, 1);
    }

    // Week window: the same structure, shortened to leave room for the footer.
    c_.dotted(kLeft, 172, right - kLeft);
    if (wp)
        snprintf(b, sizeof(b), "WEEK %u%%", s.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "WEEK --");
    c_.text(kLeft, 180, b, 2);
    reset_countdown(v, 1, true, b, sizeof(b));
    c_.text_right(right, 180, b, 2);
    bar(kLeft, 198, right - kLeft, 12, wp ? s.week_window.used_percent : -1,
        marker(pace.week_pace));
    if (pace.week_pace.valid) {
        char elapsed[16];
        format_elapsed(pace.week_pace.elapsed_s, elapsed, sizeof(elapsed));
        snprintf(b, sizeof(b), "ELAPSED %s", elapsed);
        c_.text(kLeft, 210, b, 1);
    }
}

void Screens::alert(const View &v) {
    // Only MiniMax publishes a quota threshold; the balance service raises no alert.
    constexpr int kProvider = 1;
    const auto p = provider_for(kProvider);
    const auto &s = v.model.snapshot(p);
    const uint32_t epoch = v.model.estimated_epoch(p, v.now_ms);
    const int w = c_.width(), h = c_.height(), right = w - kLeft;
    const bool exhausted = s.short_window.used_percent >= 100;
    char b[32];
    c_.text(kLeft, 12, kProviderNames[kProvider], 2, Ink::White);
    const float balance = static_cast<float>(compact_balance(v, 0));
    if (balance >= 0)
        snprintf(b, sizeof(b), "%s %.2f", kProviderNames[0], static_cast<double>(balance) / 100.0);
    else
        snprintf(b, sizeof(b), "%s --", kProviderNames[0]);
    c_.text_right(right, 12, b, 1, Ink::White);
    big_percent(kLeft, 34, s.short_window.used_percent, 132, Ink::White);
    c_.text(kLeft, 174, exhausted ? "5H EXHAUSTED" : "5H NEAR LIMIT", 2, Ink::White);
    format_short_countdown(s.short_window, epoch, v.settings.seconds, b, sizeof(b));
    c_.text(kLeft, 196, "RESET", 1, Ink::White);
    c_.text(kLeft + 44, 192, b, 2, Ink::White);
    const int rule = h - 22;
    c_.fill(kLeft, rule, w - 2 * kLeft, 2, Ink::White);
    c_.text(kLeft, rule + 6, "ANY KEY DISMISS", 1, Ink::White);
    c_.text_right(right, rule + 6,
                  v.settings.alert == static_cast<uint8_t>(AlertMode::Beep) ? "BEEP ON" : "BEEP OFF",
                  1, Ink::White);
}

void Screens::clock(const View &v) {
    const int right = c_.width() - kLeft;
    const int level_x = battery(v, right);
    const bool offline = !v.link.encrypted && !v.link.has_passkey;
    char b[32];
    if (v.clock.valid)
        snprintf(b, sizeof(b), "%s %02u/%02u", kDayNames[v.clock.weekday % 7], v.clock.month,
                 v.clock.day);
    else
        snprintf(b, sizeof(b), "NO DATE");
    c_.text(kLeft, 8, b, 2);
    const int chip_w = Canvas::text_width("OFFLINE", 2);
    if (offline)
        chip(level_x - 8 - kChipPad - chip_w, 8, "OFFLINE");
    c_.fill(kLeft, 26, right - kLeft, 2);

    // The clock: the hours and minutes fill the content column, the seconds sit in a column
    // of their own on the right so they never touch the minutes.
    constexpr int top = 38, h = 72, w = 38, t = 6;
    if (v.clock.valid)
        snprintf(b, sizeof(b), "%02u:%02u", v.clock.hour, v.clock.minute);
    else
        snprintf(b, sizeof(b), "--:--");
    c_.segments(kLeft, top, b, w, h, t);
    if (v.settings.seconds && v.clock.valid) {
        constexpr int sh = 24, sw = 15, st = 3;
        snprintf(b, sizeof(b), "%02u", v.clock.second);
        c_.segments(right - Canvas::segments_width(b, sw, st), top + h - sh, b, sw, sh, st);
    }

    // The quota band: MiniMax on top, the balance underneath.
    c_.dotted(kLeft, 132, right - kLeft);
    const auto &s = v.model.snapshot(provider_for(1));
    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    const Paces pace = paces(v, 1, sp, wp);
    c_.text(kLeft, 140, kProviderNames[1], 1);
    if (sp)
        snprintf(b, sizeof(b), "%d%%", s.short_window.used_percent);
    else
        snprintf(b, sizeof(b), "--");
    c_.text_right(right, 140, b, 1);
    c_.text(kLeft, 152, "5H", 1);
    bar(kLeft, 164, right - kLeft, 12, sp ? s.short_window.used_percent : -1,
        marker(pace.short_pace));
    c_.text(kLeft, 180, "WEEK", 1);
    if (wp)
        snprintf(b, sizeof(b), "%d%%", s.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "--");
    c_.text_right(right, 180, b, 1);
    bar(kLeft, 192, right - kLeft, 10, wp ? s.week_window.used_percent : -1,
        marker(pace.week_pace));

    const auto &d = v.model.snapshot(provider_for(0));
    c_.text(kLeft, 210, kProviderNames[0], 1);
    const int balance_x = kLeft + Canvas::text_width(kProviderNames[0], 1) + 8;
    if (balance_present(d, v.link.encrypted))
        draw_inline_balance(c_, balance_x, 206, d, right - balance_x);
    else
        c_.text(balance_x, 210, "--", 1);
}

void Screens::lyric_row(const View &v, int x, int top, int width, const char *line, Ink ink) {
    // The line stops where its whole tail fits, so its last character is always shown in full,
    // and both edges of the clip fall between glyphs rather than through one.
    const int end = CjkText::tail_start(line, width, kMusicScale);
    // The age of the line comes from the song's own timeline, so the text moves at the pace
    // the music states rather than at the pace the publication arrived.
    const uint64_t age = music_line_age_ms(v, v.now_ms);
    // A paused player keeps the line at its first word instead of scrolling away from it.
    const int offset =
        v.music.playing
            ? lyric_scroll_offset(CjkText::width(line, kMusicScale), end, age,
                                  music_line_budget_ms(v, v.now_ms))
            : 0;
    const int start = CjkText::boundary_at_or_before(line, std::min(offset, end), kMusicScale);
    const int visible = CjkText::window_width(line, start, width, kMusicScale);
    c_.clip(x, top, visible, kMusicLineHeight);
    CjkText::draw(c_, x - start, top, line, kMusicScale, ink);
    c_.reset_clip();
}

void Screens::context_row(int x, int top, int width, const char *line, Ink ink) {
    // Room for the longest line a row can hold plus its ellipsis, so a fitted line is never
    // cut by the buffer instead of by the row.
    char fitted[kMaxLyricLineBytes + 4];
    CjkText::fit(fitted, sizeof(fitted), line, width, kMusicScale);
    CjkText::draw(c_, x, top, fitted, kMusicScale, ink);
}

void Screens::music(const View &v) {
    header(v, "MUSIC");
    const int right = c_.width() - kLeft;
    const int content = right - kLeft;
    if (!v.music_valid) {
        c_.text(kLeft, kContentTop + 6, v.link.encrypted ? "WAITING FOR DATA" : "NO MUSIC", 2);
        return;
    }
    const auto &m = v.music;
    char fitted[kMaxTitleBytes + 8];
    // The song and its artist sit above the playhead, in the line height of the lyric font.
    CjkText::fit(fitted, sizeof(fitted), m.title, content, kMusicScale);
    CjkText::draw(c_, kLeft, kMusicTitleRow, fitted, kMusicScale);
    if (m.artist[0]) {
        const int paused_width = m.playing ? 0 : CjkText::width("PAUSED", kMusicScale) + 8;
        CjkText::fit(fitted, sizeof(fitted), m.artist, content - paused_width, kMusicScale);
        CjkText::draw(c_, kLeft, kMusicArtistRow, fitted, kMusicScale);
    }
    // The paused state ends the artist row in the same font, so the two share the baseline.
    if (!m.playing)
        CjkText::draw(c_, right - CjkText::width("PAUSED", kMusicScale), kMusicArtistRow, "PAUSED",
                      kMusicScale);
    // Playhead: the bar keeps clear of the elapsed/total figures on its right.
    char times[40];
    format_playhead(m.position_seconds, m.duration_seconds, times, sizeof(times));
    const int times_w = Canvas::text_width(times, 1);
    const int bar_w = std::max(60, content - times_w - 10);
    const int percent =
        m.duration_seconds > 0
            ? static_cast<int>(static_cast<uint64_t>(m.position_seconds) * 100 /
                               m.duration_seconds)
            : -1;
    bar(kLeft, kMusicBarTop, bar_w, kMusicBarHeight, std::min(percent, 100));
    c_.text_right(right, kMusicBarTop - 1, times, 1);
    // A rule separates the song from the lyrics, like the rule under the header.
    c_.fill(kLeft, kMusicRule, content, 2);
    if (m.line_count == 0) {
        c_.text(kLeft, kMusicLyricsTop + 6, v.link.encrypted ? "NO LYRICS" : "--", 2);
        return;
    }
    const int lyric_width = content - 2 * kMusicTextInset;
    // The line being sung follows the song, so it may move on before the next publication.
    const int current = music_current_line(v, v.now_ms);
    for (int row = 0; row < kMusicLyricRows; ++row) {
        const int index = current + row;
        if (index >= m.line_count)
            break;
        const int top = kMusicLyricsTop + row * kMusicLyricPitch;
        if (row == 0) {
            // The line being sung is inverted, like the chips elsewhere on the panel. The
            // band is exactly one line high, so it never covers the row below it.
            c_.fill(kLeft - kMusicTextInset, top, content + 2 * kMusicTextInset, kMusicLineHeight);
            lyric_row(v, kLeft, top, lyric_width, m.lines[index], Ink::White);
        } else {
            context_row(kLeft, top, lyric_width, m.lines[index], Ink::Black);
        }
    }
}

void Screens::settings(const View &v) {
    header(v, "SETTINGS");
    struct Row {
        const char *label;
        const char *options[5];
    };
    static const Row rows[kSettingRows] = {
        {"AUTO CYCLE", {"OFF", "30S", "1M", "5M"}},
        {"ALERT 95%", {"OFF", "PAGE", "+BEEP"}},
        {"SECONDS", {"ON", "OFF"}},
        {"ROTATION", {"0°", "90°", "180°"}},
        {"BRIGHTNESS", {"20%", "40%", "60%", "80%", "100%"}},
    };
    const auto &s = v.settings;
    const int right = c_.width() - kLeft;
    const int selected[kSettingRows] = {s.cycle, s.alert, s.seconds ? 0 : 1, s.rotation,
                                        brightness_level_index(s.brightness)};
    // Four rows of a label and an option strip, each row 42 px tall.
    for (int r = 0; r < kSettingRows; ++r) {
        const int y = 34 + 42 * r;
        const int label_top = y + 1;
        // The focused row's label is inverted in place. The box is sized for the small label
        // and stops short of the option boxes, which start further to the right.
        if (r == v.focus) {
            const int tw = Canvas::text_width(rows[r].label, 1);
            c_.fill(kLeft - 2, label_top - 2, tw + 6, kSmallLine + 4);
            c_.text(kLeft, label_top, rows[r].label, 1, Ink::White);
        } else {
            c_.text(kLeft, label_top, rows[r].label, 1);
        }
        const int n = setting_options(r);
        const int left = kLeft + 74, top = y;
        const float w = (right - left - 4.f * (n - 1)) / n;
        for (int k = 0; k < n; ++k) {
            const int x0 = left + round_px(k * (w + 4)), x1 = left + round_px(k * (w + 4) + w);
            const bool on = k == selected[r];
            if (on)
                c_.fill(x0, top, x1 - x0, 22);
            else
                c_.frame(x0, top, x1 - x0, 22, 2);
            const int tw = Canvas::text_width(rows[r].options[k], 1);
            c_.text(x0 + (x1 - x0 - tw) / 2, top + 8, rows[r].options[k], 1,
                    on ? Ink::White : Ink::Black);
        }
    }
    c_.fill(kLeft, kFooterRule, right - kLeft, 2);
    if (v.save_error) {
        c_.text(kLeft, kFooterText, "SAVE FAILED / RETRY", 1);
        return;
    }
    // The forward and back keys change the value of the focused row, the home key moves the
    // focus to the next row and holding a key leaves the settings.
    c_.text(kLeft, kFooterText, "BACK/FWD SET", 1);
    c_.text((c_.width() - Canvas::text_width("HOME ROW", 1)) / 2, kFooterText, "HOME ROW", 1);
    c_.text_right(right, kFooterText, "HOLD EXIT", 1);
}

void Screens::pairing(const View &v) {
    header(v, nullptr);
    const int right = c_.width() - kLeft;
    c_.text(kLeft, kContentTop, "PAIRING CODE", 2);
    char b[8];
    // Two groups of three digits.
    constexpr int h = 56, w = 30, t = 6;
    snprintf(b, sizeof(b), "%03lu", static_cast<unsigned long>(v.link.passkey / 1000 % 1000));
    const int first = c_.segments(kLeft, 56, b, w, h, t);
    snprintf(b, sizeof(b), "%03lu", static_cast<unsigned long>(v.link.passkey % 1000));
    c_.segments(kLeft + first + w / 2, 56, b, w, h, t);
    const int text = 56 + h + 12;
    c_.text(kLeft, text, "ENTER THIS CODE ON", 2);
    c_.text(kLeft, text + kLine + 2, "THE DESKTOP BRIDGE", 2);
    c_.fill(kLeft, kFooterRule, right - kLeft, 2);
    c_.text(kLeft, kFooterText, v.device_name, 1);
    c_.text_right(right, kFooterText, "PRESS NEW CODE", 1);
}

void Screens::ota(const View &v) {
    header(v, nullptr);
    const int right = c_.width() - kLeft;
    c_.text(kLeft, kContentTop, "FIRMWARE", 2);
    const int percent =
        v.ota.size ? static_cast<int>(std::min<uint64_t>(100, v.ota.offset * 100ULL / v.ota.size)) : 0;
    const int shown = percent / 5 * 5;
    c_.text(kLeft, 52, v.ota.version, 2);
    big_percent(kLeft, 74, shown, 60);
    bar(kLeft, 148, right - kLeft, 14, percent);
    const char *phase = v.ota.stage == OtaStage::Confirming  ? "CONFIRM UPDATE"
                        : v.ota.stage == OtaStage::Rebooting ? "RESTARTING"
                        : v.ota.stage == OtaStage::Verifying ? "VERIFYING"
                                                             : "WRITING";
    char b[64];
    snprintf(b, sizeof(b), "%s %.1f/%.1f MB", phase, v.ota.offset / 1e6, v.ota.size / 1e6);
    c_.text(kLeft, 170, b, 1);
    const bool confirming = v.ota.stage == OtaStage::Confirming;
    c_.fill(kLeft, kFooterRule, right - kLeft, 2);
    if (confirming) {
        c_.text(kLeft, kFooterText, "ANY KEY CONFIRM", 1);
        c_.text_right(right, kFooterText, "HOLD DENY", 1);
    } else {
        c_.text(kLeft, kFooterText, "DO NOT UNPLUG OR POWER OFF", 1);
    }
}

void Screens::render(const View &v) {
    const uint8_t rotation = std::min<uint8_t>(v.settings.rotation, 2);
    // A retained OTA failure banner takes precedence; the usage alert returns when it expires.
    const bool show_alert = v.alert >= 0 && v.alert < 2 && v.ota.stage == OtaStage::Idle &&
                            !v.ota.error && !v.link.has_passkey && v.page != Page::Settings;
    c_.begin(rotation, show_alert);
    // A 2 px frame 4 px inside the panel edge, like the printed border of a segment LCD.
    c_.frame(kBezel, kBezel, c_.width() - 2 * kBezel, c_.height() - 2 * kBezel, 2,
             show_alert ? Ink::White : Ink::Black);
    if (v.ota.stage != OtaStage::Idle)
        ota(v);
    else if (v.link.has_passkey)
        pairing(v);
    else if (show_alert)
        alert(v);
    else {
        switch (v.page) {
        case Page::Home:
            home(v);
            break;
        case Page::Deepseek:
            balance_page(v);
            break;
        case Page::Minimax:
            windows_page(v);
            break;
        case Page::Music:
            music(v);
            break;
        case Page::Clock:
            clock(v);
            break;
        case Page::Settings:
            settings(v);
            break;
        }
        // Settings and the passkey/OTA screens sit outside the page ring, so they get no dots.
        if (const int ring = ring_index(v.page); ring >= 0)
            page_dots(ring);
    }
    if (v.ota.error && v.ota.stage == OtaStage::Idle && !show_alert) {
        c_.fill(kLeft, c_.height() - 26, c_.width() - 2 * kLeft, kLine);
        c_.text(kLeft + 4, c_.height() - 24, v.ota.error, 1, Ink::White);
    }
}
} // namespace usage_panel::zltech
