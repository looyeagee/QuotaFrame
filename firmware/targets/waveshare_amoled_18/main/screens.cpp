#include "screens.hpp"
#include "usage_panel/cjk_font.hpp"
#include "usage_panel/panel_logic.hpp"
#include "usage_panel/screen_parts.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace usage_panel::amoled {
namespace {
// Content sits 16 px inside the 2 px bezel frame drawn 4 px from the panel edge.
constexpr int kLeft = 16, kBezel = 4;
// Pixel-font line height at the label scale.
constexpr int kLine = 14;
// Pixel-font line height at the small scale.
constexpr int kSmallLine = kLine / 2;
// Inverted chips extend this far around their text, so the text aligns with plain labels.
constexpr int kChipPad = 4;
// First content line, just below the header rule.
constexpr int kContentTop = 32;
// Smallest pace excess, in percentage points, that the detail page reports as OVER.
constexpr int kOverThreshold = 5;
// Footer rule on pages with a footer line; the footer text follows 6 px below it. Both
// anchor to the bottom edge so the three rotations keep the same page furniture.
constexpr int kFooterFromBottom = 34, kFooterTextFromBottom = 28;
// Page dots sit above the bottom edge, clear of the footer.
constexpr int kDotsFromBottom = 26;
// The portrait glass is 448 tall; the turned glass is 368 tall. Pages pick one of two
// anchor sets so the quarter-turned image still fits without a third layout to maintain.
constexpr int kTallGlassMinHeight = 430;
// Music page: lyric font at 3x (36 px rows), the song above the playhead.
constexpr int kMusicScale = 3;
constexpr int kMusicLineHeight = CjkText::line_height(kMusicScale); // 36 px
constexpr int kMusicTitleRow = 36, kMusicArtistRow = 78;
constexpr int kMusicBarTop = 122, kMusicBarHeight = 14;
constexpr int kMusicRule = 144;
constexpr int kMusicLyricsTop = 150, kMusicLyricPitch = kMusicLineHeight;
constexpr int kMusicLyricRowsMax = 8;
// The inverted lyric row extends this far past the text margin, like the status chips.
constexpr int kMusicTextInset = 4;
// Balance figure heights, largest first; the smallest one is the fallback.
constexpr int kHomeBalanceHeights[] = {88, 80, 72, 64, 56, 48, 44, 40, 36, 32};
constexpr int kHomeBalanceHeightsCompact[] = {56, 48, 44, 40, 36, 32, 28, 24};
constexpr int kBalanceHeights[] = {136, 124, 112, 100, 88, 80, 72, 64, 56, 48, 40, 32};
// Clock face proportions: seven-segment digits read correctly at the 5:9 cell ratio; the
// ladder entries pick the largest face that fits the column with the seconds beside it.
struct ClockFace {
    int w, h, t, gap, seconds_w, seconds_h, seconds_t;
};
constexpr ClockFace kClockFaceTall = {56, 100, 11, 14, 18, 32, 4};
constexpr ClockFace kClockFaceCompact = {44, 80, 10, 13, 15, 26, 4};
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
    const int y = c_.height() - kDotsFromBottom;
    for (int i = 0; i < pages; ++i) {
        if (i == page)
            c_.fill(x + 12 * i, y, 8, 8);
        else
            c_.frame(x + 12 * i, y, 8, 8, 2);
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
    // sides of it, so it stays readable over the filled part.
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
    const bool tall = c_.height() >= kTallGlassMinHeight;
    char b[32];
    const auto &d = v.model.snapshot(provider_for(0));
    const auto &m = v.model.snapshot(provider_for(1));

    // DeepSeek block: the caption row and the figure underneath it, then the sample age.
    c_.text(kLeft, kContentTop, kProviderNames[0], 2);
    int figure_h = 0;
    if (balance_present(d, v.link.encrypted)) {
        if (tall)
            figure_h = draw_balance(c_, kLeft, 58, d, right - kLeft, 88, kHomeBalanceHeights);
        else
            figure_h =
                draw_balance(c_, kLeft, 58, d, right - kLeft, 56, kHomeBalanceHeightsCompact);
    } else {
        c_.text(kLeft, 62, empty_message(v), 2);
    }
    if (age_label(v, 0, b, sizeof(b)))
        c_.text_right(right, 58 + figure_h + 6, b, 1);

    // MiniMax block: anchored to the bottom of the glass, so the shorter landscape glass
    // keeps the same order without pushing the figure off the edge.
    const int mini_top = tall ? 210 : c_.height() - 232;
    c_.dotted(kLeft, mini_top - 14, right - kLeft);
    c_.text(kLeft, mini_top, kProviderNames[1], 2);
    const bool sp = short_present(m, v.link.encrypted), wp = week_present(m, v.link.encrypted);
    if (!sp && !wp) {
        c_.text(kLeft, mini_top + 24, empty_message(v), 2);
        return;
    }
    const Paces pace = paces(v, 1, sp, wp);
    big_percent(kLeft, mini_top + 24, sp ? m.short_window.used_percent : -1, tall ? 64 : 48);
    c_.text(kLeft, mini_top + 96, "5H", 2);
    reset_countdown(v, 1, false, b, sizeof(b));
    c_.text_right(right, mini_top + 96, b, 2);
    bar(kLeft, mini_top + 120, right - kLeft, tall ? 16 : 14,
        sp ? m.short_window.used_percent : -1, marker(pace.short_pace));
    if (wp)
        snprintf(b, sizeof(b), "WEEK %u%%", m.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "WEEK --");
    c_.text(kLeft, mini_top + 146, b, 2);
    reset_countdown(v, 1, true, b, sizeof(b));
    c_.text_right(right, mini_top + 146, b, 2);
    bar(kLeft, mini_top + 170, right - kLeft, tall ? 14 : 12,
        wp ? m.week_window.used_percent : -1, marker(pace.week_pace));
}

void Screens::balance_page(const View &v) {
    const auto &s = v.model.snapshot(provider_for(0));
    const int right = c_.width() - kLeft;
    const int h = c_.height();
    char b[32];
    header(v, kProviderNames[0]);
    c_.text(kLeft, kContentTop, "BALANCE", 2);
    if (!balance_present(s, v.link.encrypted)) {
        c_.text(kLeft, 82, empty_message(v), 2);
        return;
    }
    // The figure and its age line sit centred in the room between the header rule and the
    // credit block, so the tall glass does not leave one big hole under the digits.
    const int zone_top = 46, zone_bottom = h - 140;
    if (!balance_present(s, v.link.encrypted)) {
        c_.text(kLeft, 82, empty_message(v), 2);
        return;
    }
    const int used = balance_height(s, right - kLeft, 136, kBalanceHeights);
    const int figure_top = zone_top + std::max(0, (zone_bottom - zone_top - used) / 2);
    draw_balance(c_, kLeft, figure_top, s, right - kLeft, 136, kBalanceHeights);
    if (age_label(v, 0, b, sizeof(b)))
        c_.text(kLeft, figure_top + used + 8, b, 2);
    c_.dotted(kLeft, h - 134, right - kLeft);
    // The two lines keep a gap between them and stay clear of the page dots.
    c_.text(kLeft, h - 118, "CREDIT LEFT", 2);
    c_.text(kLeft, h - 96, "ON THE ACCOUNT", 2);
}

void Screens::windows_page(const View &v) {
    const auto &s = v.model.snapshot(provider_for(1));
    const int right = c_.width() - kLeft;
    const bool tall = c_.height() >= kTallGlassMinHeight;
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
    const int fig_h = tall ? 112 : 76;
    big_percent(kLeft, 64, sp ? s.short_window.used_percent : -1, fig_h);
    if (pace.short_pace.valid && pace.short_pace.delta >= kOverThreshold) {
        // The chip keeps a gap to the figure, so it never merges with the digits.
        snprintf(b, sizeof(b), "OVER +%d%%", pace.short_pace.delta);
        chip(kLeft, 64 + fig_h + 12, b);
    }
    const int bar1_top = 64 + fig_h + (tall ? 48 : 36);
    bar(kLeft, bar1_top, right - kLeft, tall ? 26 : 20,
        sp ? s.short_window.used_percent : -1, marker(pace.short_pace));
    if (pace.short_pace.valid) {
        char elapsed[16];
        format_elapsed(pace.short_pace.elapsed_s, elapsed, sizeof(elapsed));
        snprintf(b, sizeof(b), "ELAPSED %s", elapsed);
        c_.text(kLeft, bar1_top + (tall ? 36 : 28), b, 2);
    }

    // Week window: the same structure under a rule, anchored above the page dots.
    const int week_top = tall ? c_.height() - 132 : c_.height() - 124;
    c_.dotted(kLeft, week_top - 14, right - kLeft);
    if (wp)
        snprintf(b, sizeof(b), "WEEK %u%%", s.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "WEEK --");
    c_.text(kLeft, week_top, b, 2);
    reset_countdown(v, 1, true, b, sizeof(b));
    c_.text_right(right, week_top, b, 2);
    bar(kLeft, week_top + 26, right - kLeft, tall ? 24 : 18,
        wp ? s.week_window.used_percent : -1, marker(pace.week_pace));
    if (pace.week_pace.valid) {
        char elapsed[16];
        format_elapsed(pace.week_pace.elapsed_s, elapsed, sizeof(elapsed));
        snprintf(b, sizeof(b), "ELAPSED %s", elapsed);
        c_.text(kLeft, week_top + (tall ? 60 : 50), b, 2);
    }
}

void Screens::alert(const View &v) {
    // Only MiniMax publishes a quota threshold; the balance service raises no alert.
    constexpr int kProvider = 1;
    const auto p = provider_for(kProvider);
    const auto &s = v.model.snapshot(p);
    const uint32_t epoch = v.model.estimated_epoch(p, v.now_ms);
    const int w = c_.width(), h = c_.height(), right = w - kLeft;
    const bool tall = h >= kTallGlassMinHeight;
    const bool exhausted = s.short_window.used_percent >= 100;
    char b[32];
    c_.text(kLeft, 12, kProviderNames[kProvider], 2, Ink::White);
    const float balance = static_cast<float>(compact_balance(v, 0));
    if (balance >= 0)
        snprintf(b, sizeof(b), "%s %.2f", kProviderNames[0], static_cast<double>(balance) / 100.0);
    else
        snprintf(b, sizeof(b), "%s --", kProviderNames[0]);
    c_.text_right(right, 12, b, 1, Ink::White);
    big_percent(kLeft, 40, s.short_window.used_percent, tall ? 220 : 160, Ink::White);
    c_.text(kLeft, tall ? 276 : 216, exhausted ? "5H EXHAUSTED" : "5H NEAR LIMIT", 2, Ink::White);
    format_short_countdown(s.short_window, epoch, v.settings.seconds, b, sizeof(b));
    c_.text(kLeft, h - 72, "RESET", 1, Ink::White);
    c_.text(kLeft + 44, h - 76, b, 2, Ink::White);
    const int rule = h - 22;
    c_.fill(kLeft, rule, w - 2 * kLeft, 2, Ink::White);
    c_.text(kLeft, rule + 6, "TAP OR SWIPE TO DISMISS", 1, Ink::White);
    c_.text_right(right, rule + 6,
                  v.settings.alert == static_cast<uint8_t>(AlertMode::Beep) ? "BEEP ON" : "BEEP OFF",
                  1, Ink::White);
}

void Screens::clock(const View &v) {
    const int right = c_.width() - kLeft;
    const bool tall = c_.height() >= kTallGlassMinHeight;
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

    // The clock face: 5:9 digits drawn glyph by glyph, so the gaps between them are wide
    // enough to read at this size. The seconds sit right-aligned on the face baseline.
    const ClockFace &face = tall ? kClockFaceTall : kClockFaceCompact;
    const int face_top = tall ? 38 : 36;
    if (v.clock.valid)
        snprintf(b, sizeof(b), "%02u:%02u", v.clock.hour, v.clock.minute);
    else
        snprintf(b, sizeof(b), "--:--");
    int x = kLeft;
    for (int i = 0; i < 5; ++i) {
        const char ch = b[i];
        if (ch == ':') {
            // Two square dots at the strokes' width, spread over the digit height.
            c_.fill(x, face_top + face.h * 30 / 100, face.t, face.t);
            c_.fill(x, face_top + face.h * 62 / 100, face.t, face.t);
            x += face.t + face.gap;
            continue;
        }
        c_.segments(x, face_top, &ch, face.w, face.h, face.t);
        x += face.w + face.gap;
    }
    if (v.settings.seconds && v.clock.valid) {
        snprintf(b, sizeof(b), "%02u", v.clock.second);
        c_.segments(right - Canvas::segments_width(b, face.seconds_w, face.seconds_t),
                    face_top + face.h - face.seconds_h, b, face.seconds_w, face.seconds_h,
                    face.seconds_t);
    }

    // The quota band keeps the reference target's row order: MiniMax with its percentage,
    // the 5H label directly above the short-window bar (the countdown shares that label
    // row), the week window with its bar and the balance. The rows advance with one even
    // rhythm from the dotted rule down to the page dots, so the band fills the glass
    // instead of clumping at the top and leaving a void at the bottom.
    const int band_top = tall ? 158 : 134, row_gap = tall ? 18 : 12;
    const int bar1_h = tall ? 30 : 26, bar2_h = tall ? 28 : 24;
    c_.dotted(kLeft, band_top, right - kLeft);
    const auto &s = v.model.snapshot(provider_for(1));
    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    const Paces pace = paces(v, 1, sp, wp);
    int row = band_top + 2 + row_gap;
    c_.text(kLeft, row, kProviderNames[1], 2);
    if (sp)
        snprintf(b, sizeof(b), "%d%%", s.short_window.used_percent);
    else
        snprintf(b, sizeof(b), "--");
    c_.text_right(right, row, b, 2);
    row += kLine + row_gap;
    c_.text(kLeft, row, "5H", 2);
    reset_countdown(v, 1, false, b, sizeof(b));
    c_.text_right(right, row, b, 2);
    row += kLine + row_gap;
    bar(kLeft, row, right - kLeft, bar1_h, sp ? s.short_window.used_percent : -1,
        marker(pace.short_pace));
    row += bar1_h + row_gap;
    c_.text(kLeft, row, "WEEK", 2);
    if (wp)
        snprintf(b, sizeof(b), "%d%%", s.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "--");
    c_.text_right(right, row, b, 2);
    row += kLine + row_gap;
    bar(kLeft, row, right - kLeft, bar2_h, wp ? s.week_window.used_percent : -1,
        marker(pace.week_pace));
    row += bar2_h + row_gap;
    c_.dotted(kLeft, row, right - kLeft);
    row += 2 + row_gap;
    c_.text(kLeft, row, kProviderNames[0], 2);
    const int balance_x = kLeft + Canvas::text_width(kProviderNames[0], 2) + 8;
    const auto &d = v.model.snapshot(provider_for(0));
    if (balance_present(d, v.link.encrypted))
        draw_inline_balance(c_, balance_x, row, d, right - balance_x);
    else
        c_.text(balance_x, row, "--", 2);
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
        const int paused_width = m.playing ? 0 : CjkText::width("PAUSED", kMusicScale) + 12;
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
    const int times_w = Canvas::text_width(times, 2);
    const int bar_w = std::max(60, content - times_w - 12);
    const int percent =
        m.duration_seconds > 0
            ? static_cast<int>(static_cast<uint64_t>(m.position_seconds) * 100 /
                               m.duration_seconds)
            : -1;
    bar(kLeft, kMusicBarTop, bar_w, kMusicBarHeight, std::min(percent, 100));
    c_.text_right(right, kMusicBarTop - 1, times, 2);
    // A rule separates the song from the lyrics, like the rule under the header.
    c_.fill(kLeft, kMusicRule, content, 2);
    if (m.line_count == 0) {
        c_.text(kLeft, kMusicLyricsTop + 10, v.link.encrypted ? "NO LYRICS" : "--", 2);
        return;
    }
    const int lyric_width = content - 2 * kMusicTextInset;
    // The window holds as many whole rows as the glass gives, one row per line, so a row
    // never reaches into the row below it.
    const int rows =
        std::min(kMusicLyricRowsMax,
                 std::max(1, (c_.height() - kDotsFromBottom - kMusicLyricsTop) / kMusicLyricPitch));
    // The line being sung follows the song, so it may move on before the next publication.
    const int current = music_current_line(v, v.now_ms);
    for (int row = 0; row < rows; ++row) {
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
    // Five rows of a label and an option strip, spread between the header and the footer.
    const int pitch = (c_.height() - 100) / kSettingRows;
    for (int r = 0; r < kSettingRows; ++r) {
        const int y = 48 + pitch * r;
        const int label_top = y + (pitch - kSmallLine) / 2;
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
        const int left = kLeft + 96, box_h = std::min(34, pitch - 8);
        const int top = y + (pitch - box_h) / 2;
        const float w = (right - left - 4.f * (n - 1)) / n;
        for (int k = 0; k < n; ++k) {
            const int x0 = left + round_px(k * (w + 4)), x1 = left + round_px(k * (w + 4) + w);
            const bool on = k == selected[r];
            if (on)
                c_.fill(x0, top, x1 - x0, box_h);
            else
                c_.frame(x0, top, x1 - x0, box_h, 2);
            const int tw = Canvas::text_width(rows[r].options[k], 1);
            c_.text(x0 + (x1 - x0 - tw) / 2, top + (box_h - kSmallLine) / 2, rows[r].options[k], 1,
                    on ? Ink::White : Ink::Black);
        }
    }
    const int footer_rule = c_.height() - kFooterFromBottom;
    c_.fill(kLeft, footer_rule, right - kLeft, 2);
    if (v.save_error) {
        c_.text(kLeft, footer_rule + 6, "SAVE FAILED / RETRY", 1);
        return;
    }
    // The swipe and the BOOT key change the value of the focused row, a tap moves the focus
    // to the next row and a hold leaves the settings.
    c_.text(kLeft, footer_rule + 6, "SWIPE SET", 1);
    c_.text((c_.width() - Canvas::text_width("TAP ROW", 1)) / 2, footer_rule + 6, "TAP ROW", 1);
    c_.text_right(right, footer_rule + 6, "HOLD EXIT", 1);
}

void Screens::pairing(const View &v) {
    header(v, nullptr);
    const int right = c_.width() - kLeft;
    c_.text(kLeft, kContentTop, "PAIRING CODE", 2);
    char b[8];
    // Two groups of three digits.
    constexpr int h = 96, w = 44, t = 7;
    snprintf(b, sizeof(b), "%03lu", static_cast<unsigned long>(v.link.passkey / 1000 % 1000));
    const int first = c_.segments(kLeft, 64, b, w, h, t);
    snprintf(b, sizeof(b), "%03lu", static_cast<unsigned long>(v.link.passkey % 1000));
    c_.segments(kLeft + first + w / 2, 64, b, w, h, t);
    const int text = 64 + h + 20;
    c_.text(kLeft, text, "ENTER THIS CODE ON", 2);
    c_.text(kLeft, text + kLine + 2, "THE DESKTOP BRIDGE", 2);
    const int footer_rule = c_.height() - kFooterFromBottom;
    c_.fill(kLeft, footer_rule, right - kLeft, 2);
    c_.text(kLeft, footer_rule + 6, v.device_name, 1);
    c_.text_right(right, footer_rule + 6, "TAP NEW CODE", 1);
}

void Screens::ota(const View &v) {
    header(v, nullptr);
    const int right = c_.width() - kLeft;
    c_.text(kLeft, kContentTop, "FIRMWARE", 2);
    const int percent =
        v.ota.size ? static_cast<int>(std::min<uint64_t>(100, v.ota.offset * 100ULL / v.ota.size)) : 0;
    const int shown = percent / 5 * 5;
    c_.text(kLeft, 56, v.ota.version, 2);
    big_percent(kLeft, 84, shown, 96);
    bar(kLeft, 196, right - kLeft, 16, percent);
    const char *phase = v.ota.stage == OtaStage::Confirming  ? "CONFIRM UPDATE"
                        : v.ota.stage == OtaStage::Rebooting ? "RESTARTING"
                        : v.ota.stage == OtaStage::Verifying ? "VERIFYING"
                                                             : "WRITING";
    char b[64];
    snprintf(b, sizeof(b), "%s %.1f/%.1f MB", phase, v.ota.offset / 1e6, v.ota.size / 1e6);
    c_.text(kLeft, 224, b, 1);
    const bool confirming = v.ota.stage == OtaStage::Confirming;
    const int footer_rule = c_.height() - kFooterFromBottom;
    c_.fill(kLeft, footer_rule, right - kLeft, 2);
    if (confirming) {
        c_.text(kLeft, footer_rule + 6, "TAP CONFIRMS", 1);
        c_.text_right(right, footer_rule + 6, "HOLD DENIES", 1);
    } else {
        c_.text(kLeft, footer_rule + 6, "DO NOT UNPLUG OR POWER OFF", 1);
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
} // namespace usage_panel::amoled
