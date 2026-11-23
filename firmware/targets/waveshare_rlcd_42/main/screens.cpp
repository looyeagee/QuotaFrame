#include "screens.hpp"
#include "usage_panel/cjk_font.hpp"
#include "usage_panel/panel_logic.hpp"
#include "usage_panel/screen_parts.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace usage_panel::rlcd {
namespace {
// Content sits 8 px inside the 2 px bezel frame drawn 4 px from the panel edge.
constexpr int kLeft = 14, kBezel = 4;
// Pixel-font line height at the label scale.
constexpr int kLine = 14;
// Inverted chips extend this far around their text, so the text aligns with plain labels.
constexpr int kChipPad = 4;
// First content line, 10 px below the header rule.
constexpr int kContentTop = 46;
// Smallest pace excess, in percentage points, that the detail page reports as OVER. Smaller
// deltas are within the rounding of whole-percent usage and the fixed window lengths.
constexpr int kOverThreshold = 5;
// Footer rule on pages with a footer line; the footer text follows 10 px below it.
constexpr int kFooterRule = 252, kPortraitFooterRule = 340;
// Portrait pages: the panel turned a quarter turn is 300 x 400, so the content column is
// 272 px wide and the blocks stack down the page.
constexpr int kPortraitContentTop = 44;
// Portrait detail pages: the room the balance figure is centred in, and the note below it.
constexpr int kPortraitFigureRoom = 190, kPortraitFigureTop = 80, kPortraitNoteRule = 296;
// Portrait MiniMax page: the figure, the row the pace chip takes, the bar and the elapsed
// time, then the weekly block.
constexpr int kPortraitWindowTop = 66, kPortraitWindowFigure = 96, kPortraitChipTop = 172;
constexpr int kPortraitBarTop = 204, kPortraitElapsedTop = 236, kPortraitWeekRule = 258;
constexpr int kPortraitWeekTop = 270, kPortraitWeekBarTop = 292, kPortraitWeekElapsedTop = 318;
// Portrait music page: the song, its artist, the playhead, the rule and the lyric window.
constexpr int kPortraitMusicTitle = 44, kPortraitMusicArtist = 76, kPortraitMusicBarTop = 112;
constexpr int kPortraitMusicRule = 138, kPortraitMusicLyricsTop = 150;
// The portrait lyric window holds the whole window the Bridge sends: the line being sung
// and the six lines after it.
constexpr int kPortraitMusicLyricRows = 7;
// Music page: two lyric-font lines for the song, a playhead, then the lyric window.
constexpr int kMusicScale = 2;
constexpr int kMusicLineHeight = CjkText::line_height(kMusicScale);
// "PAUSED" ends the artist row in the lyric font, which keeps the artist clear of it there.
constexpr int kPausedWidth = 76;
constexpr int kMusicBarTop = 106, kMusicBarHeight = 14;
constexpr int kMusicRule = 128;
constexpr int kMusicLyricsTop = 140, kMusicLyricPitch = 28, kMusicLyricRows = 5;
// Inverted lyric rows bleed this far past the text margin, like the status chips.
constexpr int kMusicTextInset = 4;
// Clock page summary block: the song row below the provider caption, then a rule and the lyric
// window at the music scale. Two lyric rows are enough here because the panel is not the
// dedicated music page, so the block can afford the taller glyphs.
constexpr int kSummarySongRow = 20;
constexpr int kSummaryRule = 30, kSummaryLyricTop = 36, kSummaryLyricPitch = 32;
constexpr int kSummaryLyricRows = 2;
// Sessions page: the live count and a chip per running session beside its name. Portrait gets
// the roomier pitch of the taller glass.
constexpr int kSessionsRowGap = 10;
// The rows the glass holds below the summary: seven in landscape, nine in the taller
// portrait. A longer roster keeps the rows that were touched last.
constexpr int kSessionsRowsLandscape = 7, kSessionsRowsPortrait = 9;

// Segment heights the balance figure tries, largest first; the smallest one is the
// fallback when none of them fits.
constexpr int kBalanceHeights[] = {68, 60, 52, 44, 36, 32, 28};
// The portrait column is narrower than the landscape one, so the figure there is taller
// only while the amount and its currency still fit beside each other.
constexpr int kPortraitBalanceHeights[] = {88, 80, 72, 64, 56, 48, 40, 36, 32, 28};
} // namespace

int Screens::battery(const View &v, int right, Ink ink) {
    char pct[8];
    if (v.battery < 0)
        snprintf(pct, sizeof(pct), "--%%");
    else
        snprintf(pct, sizeof(pct), "%d%%", std::clamp(v.battery, 0, 100));
    const int tw = Canvas::text_width(pct, 2);
    const int x = right - tw;
    c_.text(x, 14, pct, 2, ink);
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

void Screens::header(const View &v, const char *title, int page) {
    char time[8];
    snprintf(time, sizeof(time), "%02u:%02u", v.clock.hour, v.clock.minute);
    c_.text(kLeft, 14, title ? title : v.clock.valid ? time : "--:--", 2);
    int edge = battery(v, c_.width() - kLeft);
    const char *status = link_status(v);
    // LINKED is the normal state and is not shown; every other state gets a chip.
    if (v.link.has_passkey || v.ota.stage != OtaStage::Idle || !v.link.encrypted) {
        const int tw = Canvas::text_width(status, 2);
        edge -= 8 + kChipPad + tw;
        chip(edge, 14, status);
        edge -= kChipPad;
    }
    if (page >= 0) {
        const int pages = ring_pages(v);
        edge -= 10 + pages * 12 - 4;
        for (int i = 0; i < pages; ++i) {
            if (i == page)
                c_.fill(edge + 12 * i, 17, 8, 8);
            else
                c_.frame(edge + 12 * i, 17, 8, 8, 2);
        }
    }
    c_.fill(kLeft, 34, c_.width() - 2 * kLeft, 2);
}

void Screens::bar(int x, int y, int w, int h, int percent, float marker) {
    constexpr int kCells = 20, kGap = 2, kInset = 4;
    c_.frame(x, y, w, h, 2);
    const int cx = x + kInset, cw = w - 2 * kInset;
    const float cell = (cw - kGap * (kCells - 1.f)) / kCells;
    int lit = percent < 0 ? 0 : round_px(std::min(percent, 100) * kCells / 100.f);
    if (percent > 0 && lit == 0)
        lit = 1;
    for (int i = 0; i < lit; ++i) {
        const int x0 = cx + round_px(i * (cell + kGap));
        const int x1 = cx + round_px(i * (cell + kGap) + cell);
        c_.fill(x0, y + kInset, x1 - x0, h - 2 * kInset);
    }
    if (marker < 0)
        return;
    // A 2 px tick overhanging the bar by 4 px, with a 2 px white halo separating it from the cells.
    const int mx = x + round_px((w - 2) * marker);
    c_.fill(mx - 2, y - 4, 6, h + 8, Ink::White);
    c_.fill(mx, y - 4, 2, h + 8);
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

void Screens::balance_row(const View &v, int top) {
    const auto &s = v.model.snapshot(provider_for(0));
    const int right = c_.width() - kLeft;
    char b[32];
    c_.text(kLeft, top, kProviderNames[0], 2);
    if (!balance_present(s, v.link.encrypted)) {
        c_.text(kLeft, top + 44, empty_message(v), 2);
        return;
    }
    // Keep the amount and its currency code clear of the right edge; the row has no
    // separate caption because a long amount would collide with it. The figure ends at the
    // bottom of the row, so a shorter amount starts lower.
    const int h = balance_height(s, right - kLeft, 68, kBalanceHeights);
    draw_balance(c_, kLeft, top + 90 - h, s, right - kLeft, 68, kBalanceHeights);
    if (age_label(v, 0, b, sizeof(b)))
        c_.text(kLeft, top + 94, b, 2);
}

void Screens::windows_row(const View &v, int top) {
    const auto &s = v.model.snapshot(provider_for(1));
    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    const int x0 = 148, x1 = c_.width() - kLeft;
    c_.text(kLeft, top, kProviderNames[1], 2);
    if (!sp && !wp) {
        c_.text(x0, top + 44, empty_message(v), 2);
        return;
    }
    const Paces pace = paces(v, 1, sp, wp);
    char b[32];
    // The figure spans top + 22 to top + 86, level with the top of the 5H bar and the
    // bottom of the WEEK bar.
    big_percent(kLeft, top + 22, sp ? s.short_window.used_percent : -1, 64);
    if (age_label(v, 1, b, sizeof(b)))
        c_.text(kLeft, top + 94, b, 2);

    c_.text(x0, top, "5H", 2);
    reset_countdown(v, 1, false, b, sizeof(b));
    c_.text_right(x1, top, b, 2);
    bar(x0, top + 22, x1 - x0, 18, sp ? s.short_window.used_percent : -1, marker(pace.short_pace));

    if (wp)
        snprintf(b, sizeof(b), "WEEK %u%%", s.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "WEEK --");
    c_.text(x0, top + 50, b, 2);
    reset_countdown(v, 1, true, b, sizeof(b));
    c_.text_right(x1, top + 50, b, 2);
    bar(x0, top + 72, x1 - x0, 14, wp ? s.week_window.used_percent : -1, marker(pace.week_pace));
}

void Screens::home(const View &v) {
    // Rows of 108 px with 10 px between the header rule, each row, the separator and the bezel.
    header(v, nullptr, ring_index(v, Page::Home));
    balance_row(v, kContentTop);
    c_.dotted(kLeft, 164, c_.width() - 2 * kLeft);
    windows_row(v, 176);
}

void Screens::home_portrait(const View &v) {
    header(v, nullptr, ring_index(v, Page::Home));
    const int right = c_.width() - kLeft, w = right - kLeft;
    const auto &s = v.model.snapshot(provider_for(1));
    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    char b[32];

    balance_row(v, 44);
    c_.dotted(kLeft, 176, w);

    const int t = 224;
    c_.text(kLeft, t, kProviderNames[1], 2);
    if (!sp && !wp) {
        c_.text(kLeft, t + 60, empty_message(v), 2);
        return;
    }
    if (age_label(v, 1, b, sizeof(b)))
        c_.text_right(right, t, b, 2);
    const Paces pace = paces(v, 1, sp, wp);
    // The 5H label and countdown end level with the bottom of the figure.
    big_percent(kLeft, t + 22, sp ? s.short_window.used_percent : -1, 64);
    c_.text_right(right, t + 50, "5H", 2);
    reset_countdown(v, 1, false, b, sizeof(b));
    c_.text_right(right, t + 72, b, 2);
    bar(kLeft, t + 96, w, 18, sp ? s.short_window.used_percent : -1, marker(pace.short_pace));

    if (wp)
        snprintf(b, sizeof(b), "WEEK %u%%", s.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "WEEK --");
    c_.text(kLeft, t + 124, b, 2);
    reset_countdown(v, 1, true, b, sizeof(b));
    c_.text_right(right, t + 124, b, 2);
    bar(kLeft, t + 146, w, 14, wp ? s.week_window.used_percent : -1, marker(pace.week_pace));
}

void Screens::focus(const View &v, int i) {
    const auto &s = v.model.snapshot(provider_for(i));
    const int right = c_.width() - kLeft, column = 240;
    char b[32];
    if (i == 0) {
        header(v, kProviderNames[0], ring_index(v, Page::Deepseek));
        c_.text(kLeft, kContentTop, "BALANCE", 2);
        if (!balance_present(s, v.link.encrypted)) {
            c_.text(kLeft, 96, empty_message(v), 2);
            return;
        }
        // The figure ends at the same line as the amount caption above it.
        const int h = balance_height(s, right - kLeft, 68, kBalanceHeights);
        draw_balance(c_, kLeft, 168 - h, s, right - kLeft, 68, kBalanceHeights);
        if (age_label(v, 0, b, sizeof(b)))
            c_.text(kLeft, 186, b, 2);
        c_.text(kLeft, 212, "CREDIT LEFT ON THE ACCOUNT", 2);
        return;
    }

    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    header(v, kProviderNames[1], ring_index(v, Page::Minimax));
    const Paces pace = paces(v, 1, sp, wp);
    // Five-hour window: 80 px figure, reset countdown and pace in the right column, then its bar.
    c_.text(kLeft, kContentTop, "5H", 2);
    big_percent(kLeft, 66, sp ? s.short_window.used_percent : -1, 80);
    c_.text(column, kContentTop, "RESET", 2);
    reset_countdown(v, 1, false, b, sizeof(b));
    c_.text(column, 66, b, 3);
    if (pace.short_pace.valid && pace.short_pace.delta >= kOverThreshold) {
        snprintf(b, sizeof(b), "OVER +%d%%", pace.short_pace.delta);
        chip(column, 110, b);
    }
    bar(kLeft, 156, right - kLeft, 18, sp ? s.short_window.used_percent : -1,
        marker(pace.short_pace));
    if (pace.short_pace.valid) {
        char elapsed[16];
        format_elapsed(pace.short_pace.elapsed_s, elapsed, sizeof(elapsed));
        snprintf(b, sizeof(b), "ELAPSED %s", elapsed);
        c_.text(kLeft, 184, b, 2);
    }
    if (age_label(v, 1, b, sizeof(b)))
        c_.text_right(right, 184, b, 2);

    // Week window: the same row structure as Home with the elapsed time below its bar.
    c_.dotted(kLeft, 208, right - kLeft);
    if (wp)
        snprintf(b, sizeof(b), "WEEK %u%%", s.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "WEEK --");
    const int label = c_.text(kLeft, 218, b, 2);
    if (pace.week_pace.valid && pace.week_pace.delta >= kOverThreshold) {
        snprintf(b, sizeof(b), "OVER +%d%%", pace.week_pace.delta);
        chip(kLeft + label + 12, 218, b);
    }
    reset_countdown(v, 1, true, b, sizeof(b));
    c_.text_right(right, 218, b, 2);
    bar(kLeft, 244, right - kLeft, 14, wp ? s.week_window.used_percent : -1,
        marker(pace.week_pace));
    if (pace.week_pace.valid) {
        char elapsed[16];
        format_elapsed(pace.week_pace.elapsed_s, elapsed, sizeof(elapsed));
        snprintf(b, sizeof(b), "ELAPSED %s", elapsed);
        c_.text(kLeft, 268, b, 2);
    }
}

void Screens::focus_portrait(const View &v, int i) {
    const auto &s = v.model.snapshot(provider_for(i));
    const int right = c_.width() - kLeft, w = right - kLeft;
    char b[32];
    if (i == 0) {
        header(v, kProviderNames[0], ring_index(v, Page::Deepseek));
        c_.text(kLeft, kPortraitContentTop, "BALANCE", 2);
        if (!balance_present(s, v.link.encrypted)) {
            c_.text(kLeft, kPortraitContentTop + 40, empty_message(v), 2);
            return;
        }
        if (age_label(v, 0, b, sizeof(b)))
            c_.text_right(right, kPortraitContentTop, b, 2);
        // The figure sits in the middle of the page, so the caption above it and the note
        // below it carry the same weight.
        const int h = balance_height(s, w, kPortraitFigureRoom, kPortraitBalanceHeights);
        const int top = kPortraitFigureTop + (kPortraitFigureRoom - h) / 2;
        draw_balance(c_, kLeft, top, s, w, kPortraitFigureRoom, kPortraitBalanceHeights);
        c_.dotted(kLeft, kPortraitNoteRule, w);
        c_.text(kLeft, kPortraitNoteRule + 12, "CREDIT LEFT", 2);
        c_.text(kLeft, kPortraitNoteRule + 34, "ON THE ACCOUNT", 2);
        return;
    }

    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    header(v, kProviderNames[1], ring_index(v, Page::Minimax));
    if (!sp && !wp) {
        c_.text(kLeft, kPortraitContentTop, empty_message(v), 2);
        return;
    }
    const Paces pace = paces(v, 1, sp, wp);
    // Five-hour window: the figure with its reset countdown beside it, the pace chip on
    // its own row, then the bar and the elapsed time.
    c_.text(kLeft, kPortraitContentTop, "5H", 2);
    c_.text_right(right, kPortraitContentTop, "RESET", 2);
    big_percent(kLeft, kPortraitWindowTop, sp ? s.short_window.used_percent : -1,
                kPortraitWindowFigure);
    reset_countdown(v, 1, false, b, sizeof(b));
    c_.text_right(right, kPortraitWindowTop, b, 2);
    if (pace.short_pace.valid && pace.short_pace.delta >= kOverThreshold) {
        snprintf(b, sizeof(b), "OVER +%d%%", pace.short_pace.delta);
        chip(kLeft, kPortraitChipTop, b);
    }
    bar(kLeft, kPortraitBarTop, w, 18, sp ? s.short_window.used_percent : -1,
        marker(pace.short_pace));
    if (pace.short_pace.valid) {
        char elapsed[16];
        format_elapsed(pace.short_pace.elapsed_s, elapsed, sizeof(elapsed));
        snprintf(b, sizeof(b), "ELAPSED %s", elapsed);
        c_.text(kLeft, kPortraitElapsedTop, b, 2);
    }
    if (age_label(v, 1, b, sizeof(b)))
        c_.text_right(right, kPortraitElapsedTop, b, 2);

    // Week window: the same row structure, below the dotted rule.
    c_.dotted(kLeft, kPortraitWeekRule, w);
    if (wp)
        snprintf(b, sizeof(b), "WEEK %u%%", s.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "WEEK --");
    c_.text(kLeft, kPortraitWeekTop, b, 2);
    reset_countdown(v, 1, true, b, sizeof(b));
    c_.text_right(right, kPortraitWeekTop, b, 2);
    bar(kLeft, kPortraitWeekBarTop, w, 14, wp ? s.week_window.used_percent : -1,
        marker(pace.week_pace));
    if (pace.week_pace.valid) {
        char elapsed[16];
        format_elapsed(pace.week_pace.elapsed_s, elapsed, sizeof(elapsed));
        snprintf(b, sizeof(b), "ELAPSED %s", elapsed);
        c_.text(kLeft, kPortraitWeekElapsedTop, b, 2);
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
    c_.text(kLeft, 14, kProviderNames[kProvider], 2, Ink::White);
    const float balance = static_cast<float>(compact_balance(v, 0));
    if (balance >= 0)
        snprintf(b, sizeof(b), "%s %.2f %s", kProviderNames[0], static_cast<double>(balance) / 100.0,
                 v.model.snapshot(provider_for(0)).currency);
    else
        snprintf(b, sizeof(b), "%s --", kProviderNames[0]);
    c_.text_right(right, 14, b, 2, Ink::White);
    big_percent(kLeft, portrait_ ? 50 : 38, s.short_window.used_percent, 150, Ink::White);
    format_short_countdown(s.short_window, epoch, v.settings.seconds, b, sizeof(b));
    // The 2x RESET label shares its bottom edge with the 3x countdown.
    int reset = 230;
    if (portrait_) {
        c_.text(kLeft, 224, "5H", 2, Ink::White);
        c_.text(kLeft, 244, exhausted ? "EXHAUSTED" : "NEAR LIMIT", 3, Ink::White);
        reset = 290;
    } else {
        c_.text(kLeft, 198, exhausted ? "5H EXHAUSTED" : "5H NEAR LIMIT", 3, Ink::White);
    }
    c_.text(kLeft, reset + 7, "RESET", 2, Ink::White);
    c_.text(kLeft + 72, reset, b, 3, Ink::White);
    const int rule = h - 38;
    c_.fill(kLeft, rule, w - 2 * kLeft, 2, Ink::White);
    c_.text(kLeft, rule + 10, "ANY KEY DISMISS", 2, Ink::White);
    c_.text_right(right, rule + 10,
                  v.settings.alert == static_cast<uint8_t>(AlertMode::Beep) ? "BEEP ON" : "BEEP OFF",
                  2, Ink::White);
}

void Screens::room_readings(const View &v, int x, int limit, int preferred_gap, int min_gap) {
    char t[16], h[16];
    if (v.environment_valid) {
        snprintf(t, sizeof(t), "%.1f°C", static_cast<double>(v.temperature));
        snprintf(h, sizeof(h), "%.0f%%", static_cast<double>(v.humidity));
    } else {
        snprintf(t, sizeof(t), "--°C");
        snprintf(h, sizeof(h), "--%%");
    }
    const int tw = Canvas::text_width(t, 2), hw = Canvas::text_width(h, 2);
    const int space = limit - x;
    // The readings keep one gap each: after the date and between the two values. The gap
    // shrinks to `min_gap` when the status chip leaves little room, and the humidity is
    // dropped when even the tightest spacing does not fit before `limit`.
    if (space < tw + hw + 2 * min_gap) {
        if (space >= tw + min_gap)
            c_.text(x + std::min(preferred_gap, space - tw), 14, t, 2);
        return;
    }
    const int gap = std::clamp((space - tw - hw) / 2, min_gap, preferred_gap);
    c_.text(x + gap, 14, t, 2);
    c_.text(x + gap + tw + gap, 14, h, 2);
}

void Screens::clock(const View &v) {
    const int right = c_.width() - kLeft;
    const int level_x = battery(v, right);
    const bool offline = !v.link.encrypted && !v.link.has_passkey;
    const int chip_w = Canvas::text_width("OFFLINE", 2);
    // The chip's left edge on the top row. In portrait the page is too narrow for the chip
    // beside the date and the readings, so it moves to the bottom of the page.
    const int chip_x0 = level_x - 8 - chip_w - 2 * kChipPad;

    char b[32];
    if (v.clock.valid)
        snprintf(b, sizeof(b), "%s %02u/%02u", kDayNames[v.clock.weekday % 7], v.clock.month,
                 v.clock.day);
    else
        snprintf(b, sizeof(b), "NO DATE");
    const int date_w = c_.text(kLeft, 14, b, 2);
    // The readings share the date row and stop before the level or the status chip.
    const int limit = offline && !portrait_ ? chip_x0 - 8 : level_x - 8;
    room_readings(v, kLeft + date_w, limit, portrait_ ? 8 : 16, 8);
    if (offline) {
        if (portrait_)
            chip(right - chip_w - kChipPad, 366, "OFFLINE");
        else
            chip(chip_x0 + kChipPad, 14, "OFFLINE");
    }

    // Landscape: 116 px digits with seconds at the right; portrait: 112 px condensed digits.
    const int h = portrait_ ? 112 : 116, w = portrait_ ? 44 : 64, t = portrait_ ? 11 : 12;
    const int top = portrait_ ? 44 : 42;
    if (v.clock.valid)
        snprintf(b, sizeof(b), "%02u:%02u", v.clock.hour, v.clock.minute);
    else
        snprintf(b, sizeof(b), "--:--");
    const int width = c_.segments(kLeft, top, b, w, h, t);
    if (v.settings.seconds && v.clock.valid) {
        snprintf(b, sizeof(b), "%02u", v.clock.second);
        const int sh = portrait_ ? 36 : 40;
        c_.segments(kLeft + width + 10, top + h - sh, b, sh * 5 / 9, sh, std::max(3, sh / 8));
    }

    // The quota band: the MiniMax rows on the left, the DeepSeek balance on the right.
    const int rule = portrait_ ? 166 : 164;
    const int label = portrait_ ? 174 : 172;
    const int row5h = portrait_ ? 190 : 192;
    const int bar5h = portrait_ ? 208 : 214;
    const int rowweek = portrait_ ? 234 : 242;
    const int barweek = portrait_ ? 252 : 264;
    // The MiniMax column ends at the split in landscape and spans the page in portrait. The
    // split leaves the right column 228 px, nine glyphs of the music scale, for the provider
    // caption and the song block.
    const int mid = portrait_ ? right : 152;
    c_.dotted(kLeft, rule, c_.width() - 2 * kLeft);

    const auto &s = v.model.snapshot(provider_for(1));
    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    const Paces pace = paces(v, 1, sp, wp);
    c_.text(kLeft, label, kProviderNames[1], 2);
    c_.text(kLeft, row5h, "5H", 2);
    if (sp)
        snprintf(b, sizeof(b), "%d%%", s.short_window.used_percent);
    else
        snprintf(b, sizeof(b), "--");
    c_.text_right(mid, row5h, b, 2);
    bar(kLeft, bar5h, mid - kLeft, 18, sp ? s.short_window.used_percent : -1,
        marker(pace.short_pace));
    c_.text(kLeft, rowweek, "WEEK", 2);
    if (wp)
        snprintf(b, sizeof(b), "%d%%", s.week_window.used_percent);
    else
        snprintf(b, sizeof(b), "--");
    c_.text_right(mid, rowweek, b, 2);
    bar(kLeft, barweek, mid - kLeft, 14, wp ? s.week_window.used_percent : -1,
        marker(pace.week_pace));

    // The balance shares its provider label row, so the space below the caption carries the
    // song being played: the song row and the line being sung with the one after it.
    const auto &d = v.model.snapshot(provider_for(0));
    if (portrait_) {
        constexpr int kBalanceRow = 272;
        c_.text(kLeft, kBalanceRow, kProviderNames[0], 2);
        const int balance_x = kLeft + Canvas::text_width(kProviderNames[0], 2) + 8;
        draw_inline_balance(c_, balance_x, kBalanceRow, d, right - balance_x);
        now_playing_summary(v, kLeft, kBalanceRow + kSummarySongRow, right - kLeft,
                            kSummaryLyricRows);
    } else {
        // The caption row holds the provider name and its balance at the label line height.
        const int column = mid + 10;
        c_.text(column, label, kProviderNames[0], 2);
        const int balance_x = column + Canvas::text_width(kProviderNames[0], 2) + 8;
        draw_inline_balance(c_, balance_x, label, d, right - balance_x);
        now_playing_summary(v, column, label + kSummarySongRow, right - column, kSummaryLyricRows);
    }
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

void Screens::now_playing_summary(const View &v, int x, int top, int width, int rows) {
    if (!v.music_valid) {
        c_.text(x, top + 2, v.link.encrypted ? "WAITING FOR DATA" : "NO MUSIC", 2);
        return;
    }
    const auto &m = v.music;
    // The longest head joins a full title and a full artist around the separator.
    char head[2 * kMaxTitleBytes + 8], fitted[2 * kMaxTitleBytes + 8];
    if (m.artist[0])
        snprintf(head, sizeof(head), "%s\u30fb%s", m.title, m.artist);
    else
        snprintf(head, sizeof(head), "%s", m.title);
    // The paused marker ends the row at the label scale, which leaves the song six glyphs
    // beside it instead of five.
    const int paused_width = m.playing ? 0 : Canvas::text_width("PAUSED", 1) + 8;
    CjkText::fit(fitted, sizeof(fitted), head, width - paused_width, kMusicScale);
    CjkText::draw(c_, x, top, fitted, kMusicScale);
    if (!m.playing)
        c_.text_right(x + width, top + CjkText::line_height(kMusicScale) - 7, "PAUSED", 1);
    c_.fill(x, top + kSummaryRule, width, 2);
    if (m.line_count == 0) {
        c_.text(x, top + kSummaryLyricTop, "NO LYRICS", 2);
        return;
    }
    // The line being sung follows the song, so it may move on before the next publication.
    const int current = music_current_line(v, v.now_ms);
    for (int row = 0; row < rows; ++row) {
        const int index = current + row;
        if (index >= m.line_count)
            break;
        const int lyric_width = width - 2 * kMusicTextInset;
        const int row_top = top + kSummaryLyricTop + row * kSummaryLyricPitch;
        if (row == 0) {
            // The line being sung is inverted, like the lyric window on the music page.
            c_.fill(x - kMusicTextInset, row_top - 2, width + 2 * kMusicTextInset,
                    CjkText::line_height(kMusicScale) + 4);
            lyric_row(v, x, row_top, lyric_width, m.lines[index], Ink::White);
        } else {
            context_row(x, row_top, lyric_width, m.lines[index], Ink::Black);
        }
    }
}

void Screens::music(const View &v) {
    header(v, "MUSIC", ring_index(v, Page::Music));
    const int right = c_.width() - kLeft;
    const int content = right - kLeft;
    if (!v.music_valid) {
        c_.text(kLeft, kContentTop, v.link.encrypted ? "WAITING FOR DATA" : "NO MUSIC", 2);
        return;
    }
    const auto &m = v.music;
    char fitted[kMaxTitleBytes + 8];
    // The song and its artist sit above the playhead, in the line height of the lyric font.
    CjkText::fit(fitted, sizeof(fitted), m.title, content, kMusicScale);
    CjkText::draw(c_, kLeft, 46, fitted, kMusicScale);
    if (m.artist[0]) {
        CjkText::fit(fitted, sizeof(fitted), m.artist, content - kPausedWidth, kMusicScale);
        CjkText::draw(c_, kLeft, 74, fitted, kMusicScale);
    }
    // The paused state ends the artist row in the same font, so the two share the baseline.
    if (!m.playing)
        CjkText::draw(c_, right - CjkText::width("PAUSED", kMusicScale), 74, "PAUSED",
                      kMusicScale);
    // Playhead: the bar keeps 12 px clear of the elapsed/total figures on its right.
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
    c_.text_right(right, kMusicBarTop + 1, times, 2);
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
            // The line being sung is inverted, like the chips elsewhere on the panel.
            c_.fill(kLeft - kMusicTextInset, top - 2, content + 2 * kMusicTextInset,
                    kMusicLineHeight + 4);
            lyric_row(v, kLeft, top, lyric_width, m.lines[index], Ink::White);
        } else {
            context_row(kLeft, top, lyric_width, m.lines[index], Ink::Black);
        }
    }
}

void Screens::music_portrait(const View &v) {
    header(v, "MUSIC", ring_index(v, Page::Music));
    const int right = c_.width() - kLeft;
    const int content = right - kLeft;
    if (!v.music_valid) {
        c_.text(kLeft, kPortraitContentTop + 6, v.link.encrypted ? "WAITING FOR DATA" : "NO MUSIC",
                2);
        return;
    }
    const auto &m = v.music;
    char fitted[kMaxTitleBytes + 8];
    // The song and its artist sit above the playhead, in the line height of the lyric font.
    CjkText::fit(fitted, sizeof(fitted), m.title, content, kMusicScale);
    CjkText::draw(c_, kLeft, kPortraitMusicTitle, fitted, kMusicScale);
    if (m.artist[0]) {
        CjkText::fit(fitted, sizeof(fitted), m.artist, content - kPausedWidth, kMusicScale);
        CjkText::draw(c_, kLeft, kPortraitMusicArtist, fitted, kMusicScale);
    }
    // The paused state ends the artist row in the same font, so the two share the baseline.
    if (!m.playing)
        CjkText::draw(c_, right - CjkText::width("PAUSED", kMusicScale), kPortraitMusicArtist,
                      "PAUSED", kMusicScale);
    // Playhead: the bar keeps 12 px clear of the elapsed/total figures on its right.
    char times[40];
    format_playhead(m.position_seconds, m.duration_seconds, times, sizeof(times));
    const int times_w = Canvas::text_width(times, 2);
    const int bar_w = std::max(60, content - times_w - 12);
    const int percent =
        m.duration_seconds > 0
            ? static_cast<int>(static_cast<uint64_t>(m.position_seconds) * 100 /
                               m.duration_seconds)
            : -1;
    bar(kLeft, kPortraitMusicBarTop, bar_w, kMusicBarHeight, std::min(percent, 100));
    c_.text_right(right, kPortraitMusicBarTop + 1, times, 2);
    // A rule separates the song from the lyrics, like the rule under the header.
    c_.fill(kLeft, kPortraitMusicRule, content, 2);
    if (m.line_count == 0) {
        c_.text(kLeft, kPortraitMusicLyricsTop + 6, v.link.encrypted ? "NO LYRICS" : "--", 2);
        return;
    }
    const int lyric_width = content - 2 * kMusicTextInset;
    // The line being sung follows the song, so it may move on before the next publication.
    const int current = music_current_line(v, v.now_ms);
    for (int row = 0; row < kPortraitMusicLyricRows; ++row) {
        const int index = current + row;
        if (index >= m.line_count)
            break;
        const int top = kPortraitMusicLyricsTop + row * kMusicLyricPitch;
        if (row == 0) {
            // The line being sung is inverted, like the chips elsewhere on the panel.
            c_.fill(kLeft - kMusicTextInset, top - 2, content + 2 * kMusicTextInset,
                    kMusicLineHeight + 4);
            lyric_row(v, kLeft, top, lyric_width, m.lines[index], Ink::White);
        } else {
            context_row(kLeft, top, lyric_width, m.lines[index], Ink::Black);
        }
    }
}

void Screens::sessions(const View &v) {
    header(v, "SESSIONS", ring_index(v, Page::Sessions));
    const int right = c_.width() - kLeft;
    if (!v.sessions_valid) {
        c_.text(kLeft, kContentTop, v.link.encrypted ? "WAITING FOR DATA" : "NO SESSIONS", 2);
        return;
    }
    const bool portrait = portrait_;
    const int summary_top = portrait ? kPortraitContentTop : kContentTop;
    const int row_top = summary_top + kLine + 2 * kChipPad + 4;
    const int pitch = portrait ? 30 : 26;
    int busy = 0;
    int waiting = 0;
    for (int i = 0; i < v.sessions.live_count; ++i) {
        if (v.sessions.live[i].state == SessionState::Busy)
            busy += 1;
        else if (v.sessions.live[i].state == SessionState::Waiting)
            waiting += 1;
    }
    char summary[44];
    // A session that waits for its reader is the one that spends the reader's time, so the
    // summary counts those first and the working ones only when none waits.
    if (waiting > 0)
        snprintf(summary, sizeof(summary), "%d LIVE / %d WAIT", v.sessions.live_count, waiting);
    else
        snprintf(summary, sizeof(summary), "%d LIVE / %d BUSY", v.sessions.live_count, busy);
    c_.text(kLeft, summary_top, summary, 2);
    // A session that is working or waiting is an inverted chip like the status chips; one
    // that is up keeps the same geometry as plain text, so the names line up across states.
    // The chips share one width, so the rows stay in a column.
    // The glass holds a fixed number of rows; a longer roster keeps the rows that were
    // touched last, which are the ones the reader is waiting for.
    const int rows = std::min<int>(v.sessions.live_count,
                                    portrait ? kSessionsRowsPortrait : kSessionsRowsLandscape);
    int chip_text = 0;
    for (int i = 0; i < rows; ++i)
        chip_text = std::max(
            chip_text, Canvas::text_width(session_state_word(v.sessions.live[i].state), 2));
    const int chip_w = chip_text + 2 * kChipPad;
    const int name_x = kLeft + chip_w + kSessionsRowGap;
    for (int i = 0; i < rows; ++i) {
        const int top = row_top + pitch * i;
        const SessionState state = v.sessions.live[i].state;
        const char *word = session_state_word(state);
        if (state == SessionState::Busy || state == SessionState::Waiting) {
            chip(kLeft, top, word);
        } else {
            c_.text(kLeft, top, word, 2);
        }
        // A session that waits says what it waits for, at the far end of its row.
        const char *reason = waiting_reason_word(v.sessions.live[i].waiting_for);
        const int reason_w = reason[0] ? CjkText::width(reason, 1) : 0;
        const int reason_x = right - reason_w;
        char fitted[kMaxSessionTextBytes + 4];
        // The name shares the label's top, so the two rows of the block line up.
        CjkText::fit(fitted, sizeof(fitted), v.sessions.live[i].name,
                     reason_x - name_x - (reason_w ? kSessionsRowGap : 0), 1);
        CjkText::draw(c_, name_x, top, fitted, 1);
        if (reason_w)
            CjkText::draw(c_, reason_x, top, reason, 1);
    }
}

void Screens::settings(const View &v) {
    header(v, "SETTINGS");
    struct Row {
        const char *label;
        const char *options[6];
    };
    static const Row rows[kSettingRows] = {
        {"AUTO CYCLE", {"OFF", "30S", "1M", "5M"}},
        {"ALERT 95%", {"OFF", "PAGE", "+BEEP"}},
        {"SECONDS", {"ON", "OFF"}},
        {"ROTATION", {"LAND", "PORT", "FLIP"}},
        // Not drawn: the reflective panel has no dimmer. The row keeps the same six steps as
        // the dimmable targets so the ladder has one shape across the repository.
        {"BRIGHTNESS", {"OFF", "20%", "40%", "60%", "80%", "100%"}},
    };
    const auto &s = v.settings;
    const int right = c_.width() - kLeft;
    const int selected[kSettingRows] = {s.cycle, s.alert, s.seconds ? 0 : 1, s.rotation,
                                        brightness_level_index(s.brightness)};
    // The reflective panel has no dimmer, so its view never offers the brightness row.
    for (int r = 0; r < v.setting_rows; ++r) {
        // Landscape puts the options beside the label; portrait puts them below it.
        const int y = portrait_ ? 44 + 74 * r : kContentTop + 48 * r;
        const int label_top = portrait_ ? y + 4 : y + 11;
        // The focused row's label is inverted in place.
        if (r == v.focus)
            chip(kLeft, label_top, rows[r].label);
        else
            c_.text(kLeft, label_top, rows[r].label, 2);
        const int n = setting_options(r);
        const int left = portrait_ ? kLeft : 148, top = portrait_ ? y + 30 : y;
        const float w = (right - left - 4.f * (n - 1)) / n;
        for (int k = 0; k < n; ++k) {
            const int x0 = left + round_px(k * (w + 4)), x1 = left + round_px(k * (w + 4) + w);
            const bool on = k == selected[r];
            if (on)
                c_.fill(x0, top, x1 - x0, 36);
            else
                c_.frame(x0, top, x1 - x0, 36, 2);
            const int tw = Canvas::text_width(rows[r].options[k], 2);
            c_.text(x0 + (x1 - x0 - tw) / 2, top + 11, rows[r].options[k], 2,
                    on ? Ink::White : Ink::Black);
        }
    }
    const int rule = portrait_ ? kPortraitFooterRule : kFooterRule;
    c_.fill(kLeft, rule, right - kLeft, 2);
    if (portrait_) {
        c_.text(kLeft, rule + 10, v.save_error ? "SAVE FAILED" : "L/R VALUE", 2);
        c_.text_right(right, rule + 10, v.save_error ? "" : "BOTH ROW", 2);
        c_.text(kLeft, rule + 30, v.save_error ? "ANY KEY RETRY" : "HOLD EXIT", 2);
    } else if (v.save_error) {
        c_.text(kLeft, rule + 10, "SAVE FAILED / RETRY", 2);
    } else {
        // The left and right keys change the value, the chord of both moves the focus row.
        c_.text(kLeft, rule + 10, "L/R VALUE", 2);
        c_.text((c_.width() - Canvas::text_width("BOTH ROW", 2)) / 2, rule + 10, "BOTH ROW", 2);
        c_.text_right(right, rule + 10, "HOLD EXIT", 2);
    }
}

void Screens::pairing(const View &v) {
    header(v, nullptr);
    const int right = c_.width() - kLeft;
    c_.text(kLeft, kContentTop, "PAIRING CODE", 2);
    char b[8];
    // Two groups of three digits; landscape 80 px digits, portrait 64 px.
    const int h = portrait_ ? 64 : 80, w = portrait_ ? 36 : 46, t = portrait_ ? 7 : 9;
    snprintf(b, sizeof(b), "%03lu", static_cast<unsigned long>(v.link.passkey / 1000 % 1000));
    const int first = c_.segments(kLeft, 68, b, w, h, t);
    snprintf(b, sizeof(b), "%03lu", static_cast<unsigned long>(v.link.passkey % 1000));
    c_.segments(kLeft + first + w / 2, 68, b, w, h, t);
    const int text = 68 + h + 20;
    c_.text(kLeft, text, "ENTER THIS CODE", 2);
    c_.text(kLeft, text + 20, "ON THE DESKTOP BRIDGE", 2);
    if (portrait_) {
        c_.fill(kLeft, kPortraitFooterRule, right - kLeft, 2);
        c_.text(kLeft, kPortraitFooterRule + 10, v.device_name, 2);
        c_.text(kLeft, kPortraitFooterRule + 30, "ANY KEY NEW CODE", 2);
        return;
    }
    c_.fill(kLeft, kFooterRule, right - kLeft, 2);
    c_.text(kLeft, kFooterRule + 10, v.device_name, 2);
    c_.text_right(right, kFooterRule + 10, "ANY KEY NEW CODE", 2);
}

void Screens::ota(const View &v) {
    header(v, nullptr);
    const int right = c_.width() - kLeft;
    c_.text(kLeft, kContentTop, "FIRMWARE", 2);
    const int percent =
        v.ota.size ? static_cast<int>(std::min<uint64_t>(100, v.ota.offset * 100ULL / v.ota.size)) : 0;
    const int shown = percent / 5 * 5, figure = big_percent_width(72);
    const int available = portrait_ ? right - kLeft : right - kLeft - figure - 12;
    const int version_scale = Canvas::text_width(v.ota.version, 3) <= available ? 3
                              : Canvas::text_width(v.ota.version, 2) <= available ? 2 : 1;
    c_.text(kLeft, 66, v.ota.version, version_scale);
    if (portrait_)
        big_percent(kLeft, 104, shown, 72);
    else
        big_percent(right - figure, kContentTop, shown, 72);
    const int y = portrait_ ? 200 : 142;
    bar(kLeft, y, right - kLeft, 16, percent);
    const char *phase = v.ota.stage == OtaStage::Confirming  ? "CONFIRM UPDATE"
                        : v.ota.stage == OtaStage::Rebooting ? "RESTARTING"
                        : v.ota.stage == OtaStage::Verifying ? "VERIFYING"
                                                             : "WRITING";
    char b[64];
    snprintf(b, sizeof(b), "%s %.1f/%.1f MB", phase, v.ota.offset / 1e6, v.ota.size / 1e6);
    c_.text(kLeft, y + 28, b, 2);
    const bool confirming = v.ota.stage == OtaStage::Confirming;
    if (portrait_) {
        c_.fill(kLeft, kPortraitFooterRule, right - kLeft, 2);
        c_.text(kLeft, kPortraitFooterRule + 10, confirming ? "ANY KEY CONFIRM" : "DO NOT UNPLUG",
                2);
        c_.text(kLeft, kPortraitFooterRule + 30, confirming ? "HOLD DENY" : "OR POWER OFF", 2);
        return;
    }
    c_.fill(kLeft, kFooterRule, right - kLeft, 2);
    c_.text(kLeft, kFooterRule + 10,
            confirming ? "ANY KEY CONFIRM / HOLD DENY" : "DO NOT UNPLUG OR POWER OFF", 2);
}

void Screens::render(const View &v) {
    const uint8_t rotation = std::min<uint8_t>(v.settings.rotation, 2);
    portrait_ = rotation == static_cast<uint8_t>(Rotation::Portrait);
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
    else
        switch (v.page) {
        case Page::Home:
            portrait_ ? home_portrait(v) : home(v);
            break;
        case Page::Deepseek:
            portrait_ ? focus_portrait(v, 0) : focus(v, 0);
            break;
        case Page::Minimax:
            portrait_ ? focus_portrait(v, 1) : focus(v, 1);
            break;
        case Page::Sessions:
            sessions(v);
            break;
        case Page::Music:
            portrait_ ? music_portrait(v) : music(v);
            break;
        case Page::Clock:
            clock(v);
            break;
        case Page::Settings:
            settings(v);
            break;
        }
    if (v.ota.error && v.ota.stage == OtaStage::Idle && !show_alert) {
        c_.fill(kLeft, c_.height() - 40, c_.width() - 2 * kLeft, kLine + 10);
        c_.text(kLeft + 6, c_.height() - 35, v.ota.error, 2, Ink::White);
    }
}
} // namespace usage_panel::rlcd
