#include "screens.hpp"
#include "cjk_font.hpp"
#include "panel_logic.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace usage_panel::rlcd {
namespace {
constexpr const char *kNames[2] = {"DEEPSEEK", "MINIMAX"};
constexpr const char *kDays[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
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
// Music page: two lyric-font lines for the song, a playhead, then the lyric window.
constexpr int kMusicScale = 2;
constexpr int kMusicLineHeight = CjkText::line_height(kMusicScale);
// "PAUSED" at the label scale keeps the artist clear of it on the same row.
constexpr int kPausedWidth = 76;
constexpr int kMusicBarTop = 106, kMusicBarHeight = 14;
constexpr int kMusicRule = 128;
constexpr int kMusicLyricsTop = 140, kMusicLyricPitch = 28, kMusicLyricRows = 5;
// Inverted lyric rows bleed this far past the text margin, like the status chips.
constexpr int kMusicTextInset = 4;

Provider provider_of(int i) { return provider_for(i); }

/** Whether the cached balance may be drawn: retained values survive a disconnect. */
bool balance_present(const ProviderSnapshot &s, bool encrypted) {
    return s.has_valid_data && s.has_balance && (s.latest_balance_present || !encrypted);
}

/**
 * Formats a balance in whole cents as "12.34".
 *
 * From 1000 units the decimals are dropped: at five glyphs the segment field is
 * already 244 px wide, and the panel's content area is 372 px.
 */
void format_balance(uint32_t cents, char *out, size_t size) {
    if (cents >= 100000)
        snprintf(out, size, "%lu", static_cast<unsigned long>((cents + 50) / 100));
    else
        snprintf(out, size, "%lu.%02lu", static_cast<unsigned long>(cents / 100),
                 static_cast<unsigned long>(cents % 100));
}

/**
 * Segment height that keeps the amount of `s` and its currency inside `max_width` at the
 * 5:9 cell ratio, never taller than `max_height`.
 */
int balance_height(const ProviderSnapshot &s, int max_width, int max_height) {
    char amount[24];
    format_balance(s.balance_cents, amount, sizeof(amount));
    const int cur_w = Canvas::text_width(s.currency, 2);
    for (int h : {68, 60, 52, 44, 36, 32, 28}) {
        if (h > max_height)
            continue;
        const int w = h * 5 / 9;
        // The currency sits 6 px after the amount, so both have to fit.
        if (Canvas::segments_width(amount, w, std::max(3, h / 8)) + 6 + cur_w <= max_width)
            return h;
    }
    return 28;
}

/**
 * Draws the balance amount and its currency with the figure top at `top`.
 *
 * @return the figure height, so a caller can align the currency or a label with it.
 */
int draw_balance(Canvas &c, int x, int top, const ProviderSnapshot &s, int max_width,
                 int max_height = 68) {
    char amount[24];
    format_balance(s.balance_cents, amount, sizeof(amount));
    const int h = balance_height(s, max_width, max_height);
    const int w = h * 5 / 9, t = std::max(3, h / 8);
    const int width = c.segments(x, top, amount, w, h, t);
    const int cur_w = Canvas::text_width(s.currency, 2);
    if (width + 6 + cur_w <= max_width)
        c.text(x + width + 6, top + h - 14, s.currency, 2);
    return h;
}

int round_px(float v) { return static_cast<int>(std::lround(v)); }

/** Playhead as "1:05 / 4:35", or with an hours field for long tracks. */
void format_playhead(uint32_t position, uint32_t duration, char *out, size_t size) {
    // Longest form: two ten-digit fields around " / " plus the colon separators.
    char text[40];
    if (duration == 0) {
        snprintf(text, sizeof(text), "-:-- / -:--");
    } else if (duration >= 3600) {
        snprintf(text, sizeof(text), "%lu:%02lu:%02lu / %lu:%02lu:%02lu",
                 static_cast<unsigned long>(position / 3600),
                 static_cast<unsigned long>(position / 60 % 60),
                 static_cast<unsigned long>(position % 60),
                 static_cast<unsigned long>(duration / 3600),
                 static_cast<unsigned long>(duration / 60 % 60),
                 static_cast<unsigned long>(duration % 60));
    } else {
        snprintf(text, sizeof(text), "%lu:%02lu / %lu:%02lu",
                 static_cast<unsigned long>(position / 60),
                 static_cast<unsigned long>(position % 60),
                 static_cast<unsigned long>(duration / 60),
                 static_cast<unsigned long>(duration % 60));
    }
    snprintf(out, size, "%s", text);
}

/** Balance in cents for the balance service, or -1 when it has none to show. */
long long compact_balance(const View &v, int i) {
    const auto &s = v.model.snapshot(provider_of(i));
    return balance_present(s, v.link.encrypted) ? static_cast<long long>(s.balance_cents) : -1;
}

/** Empty-row message when neither window has a value. */
const char *empty_message(const View &v) {
    return v.link.encrypted ? "WAITING FOR DATA" : "NO DATA";
}

/** Offline or absent windows have no live reset countdown. */
void reset_countdown(const View &v, int i, bool week, char *out, size_t size) {
    const auto provider = provider_of(i);
    const auto &s = v.model.snapshot(provider);
    const bool present = week ? week_present(s, v.link.encrypted)
                              : short_present(s, v.link.encrypted);
    if (!v.link.encrypted || !present) {
        snprintf(out, size, "--");
        return;
    }
    const uint32_t epoch = v.model.estimated_epoch(provider, v.now_ms);
    if (week)
        format_week_countdown(s.week_window, epoch, out, size);
    else
        format_short_countdown(s.short_window, epoch, v.settings.seconds, out, size);
}

/** Sample age label; false when the sample is under a minute old and needs no label. */
bool age_label(const View &v, int i, char *out, size_t size) {
    const auto &s = v.model.snapshot(provider_of(i));
    if (!s.has_valid_data) {
        snprintf(out, size, "NO DATA");
        return true;
    }
    const uint32_t epoch = v.model.estimated_epoch(provider_of(i), v.now_ms);
    const uint32_t age = epoch > s.sampled_at ? epoch - s.sampled_at : 0;
    format_age(age, out, size);
    return age >= 60;
}

/** Pace markers for the two windows of one provider; invalid while offline or absent. */
struct Paces {
    Pace short_pace, week_pace;
};
Paces paces(const View &v, int i, bool sp, bool wp) {
    const auto p = provider_of(i);
    const auto &s = v.model.snapshot(p);
    const bool enc = v.link.encrypted;
    const uint32_t epoch = v.model.estimated_epoch(p, v.now_ms);
    return {enc && sp ? compute_pace(s.short_window, epoch, kShortWindowSeconds) : Pace{},
            enc && wp ? compute_pace(s.week_window, epoch, kWeekWindowSeconds) : Pace{}};
}

float marker(const Pace &pace) { return pace.valid ? pace_fraction(pace) : -1.f; }
} // namespace

const char *link_status(const View &v) {
    if (v.link.has_passkey)
        return "PAIRING";
    if (v.ota.stage != OtaStage::Idle)
        return "UPDATING";
    return v.link.encrypted ? "LINKED" : "OFFLINE";
}

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
        const int pages = ring_pages();
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

namespace {
struct Digits {
    int w, t;
};
/** Seven-segment proportions for a given digit height. */
Digits digits_for(int h) { return {h * 5 / 9, std::max(3, h / 10)}; }
/** The percent sign is 3/8 of a digit wide plus one stroke, bottom-aligned. */
int percent_width(int h) { return digits_for(h).w * 3 / 8 + digits_for(h).t; }
/**
 * Space between the digit field and the percent sign: a third of a digit, so a 1 in the last
 * cell (lit on the right-hand side of its cell) does not read as touching the sign.
 */
int percent_gap(int h) { return digits_for(h).w / 3; }

/** Percent sign drawn with the stroke thickness of the digits: two square dots and a diagonal. */
void percent_sign(Canvas &c, int x, int bottom, int h, Ink ink) {
    const Digits d = digits_for(h);
    const int w = percent_width(h), height = h * 45 / 100, top = bottom - height;
    const int dot = d.t;
    c.fill(x, top, dot, dot, ink);
    c.fill(x + w - dot, bottom - dot, dot, dot, ink);
    const int stroke = d.t;
    for (int y = top; y < bottom; ++y) {
        const int cx = x + (w - stroke) * (bottom - 1 - y) / (height - 1);
        c.fill(cx, y, stroke, 1, ink);
    }
}
} // namespace

namespace {
/**
 * A fixed field of a hundreds half digit and two digit cells, right-aligned like a segment LCD:
 * 5 shows as a lone digit in the last cell, 100 lights the half digit.
 */
void percent_digits(int percent, char *out, size_t size) {
    if (percent < 0)
        snprintf(out, size, "h--");
    else if (percent >= 100)
        snprintf(out, size, "H00");
    else
        snprintf(out, size, "h%2d", percent);
}
} // namespace

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
    const auto &s = v.model.snapshot(provider_of(0));
    const int right = c_.width() - kLeft;
    char b[32];
    c_.text(kLeft, top, kNames[0], 2);
    if (!balance_present(s, v.link.encrypted)) {
        c_.text(kLeft, top + 44, empty_message(v), 2);
        return;
    }
    // Keep the amount and its currency code clear of the right edge; the row has no
    // separate caption because a long amount would collide with it. The figure ends at the
    // bottom of the row, so a shorter amount starts lower.
    const int h = balance_height(s, right - kLeft, 68);
    draw_balance(c_, kLeft, top + 90 - h, s, right - kLeft);
    if (age_label(v, 0, b, sizeof(b)))
        c_.text(kLeft, top + 94, b, 2);
}

void Screens::windows_row(const View &v, int top) {
    const auto &s = v.model.snapshot(provider_of(1));
    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    const int x0 = 148, x1 = c_.width() - kLeft;
    c_.text(kLeft, top, kNames[1], 2);
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
    header(v, nullptr, ring_index(Page::Home));
    balance_row(v, kContentTop);
    c_.dotted(kLeft, 164, c_.width() - 2 * kLeft);
    windows_row(v, 176);
}

void Screens::home_portrait(const View &v) {
    header(v, nullptr);
    const int right = c_.width() - kLeft, w = right - kLeft;
    const auto &s = v.model.snapshot(provider_of(1));
    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    char b[32];

    balance_row(v, 44);
    c_.dotted(kLeft, 176, w);

    const int t = 224;
    c_.text(kLeft, t, kNames[1], 2);
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
    const auto &s = v.model.snapshot(provider_of(i));
    const int right = c_.width() - kLeft, column = 240;
    char b[32];
    if (i == 0) {
        header(v, kNames[0], ring_index(Page::Deepseek));
        c_.text(kLeft, kContentTop, "BALANCE", 2);
        if (!balance_present(s, v.link.encrypted)) {
            c_.text(kLeft, 96, empty_message(v), 2);
            return;
        }
        // The figure ends at the same line as the amount caption above it.
        const int h = balance_height(s, right - kLeft, 68);
        draw_balance(c_, kLeft, 168 - h, s, right - kLeft);
        if (age_label(v, 0, b, sizeof(b)))
            c_.text(kLeft, 186, b, 2);
        c_.text(kLeft, 212, "CREDIT LEFT ON THE ACCOUNT", 2);
        return;
    }

    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    header(v, kNames[1], ring_index(Page::Minimax));
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

void Screens::alert(const View &v) {
    // Only MiniMax publishes a quota threshold; the balance service raises no alert.
    constexpr int kProvider = 1;
    const auto p = provider_of(kProvider);
    const auto &s = v.model.snapshot(p);
    const uint32_t epoch = v.model.estimated_epoch(p, v.now_ms);
    const int w = c_.width(), h = c_.height(), right = w - kLeft;
    const bool exhausted = s.short_window.used_percent >= 100;
    char b[32];
    c_.text(kLeft, 14, kNames[kProvider], 2, Ink::White);
    const float balance = static_cast<float>(compact_balance(v, 0));
    if (balance >= 0)
        snprintf(b, sizeof(b), "%s %.2f %s", kNames[0], static_cast<double>(balance) / 100.0,
                 v.model.snapshot(provider_of(0)).currency);
    else
        snprintf(b, sizeof(b), "%s --", kNames[0]);
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
    c_.text(kLeft, rule + 10, "KEY DISMISS", 2, Ink::White);
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
        snprintf(b, sizeof(b), "%s %02u/%02u", kDays[v.clock.weekday % 7], v.clock.month,
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
    const int row5h = portrait_ ? 196 : 192;
    const int bar5h = portrait_ ? 218 : 214;
    const int rowweek = portrait_ ? 254 : 242;
    const int barweek = portrait_ ? 276 : 264;
    // The MiniMax column ends at the split in landscape and spans the page in portrait.
    // The split leaves the DeepSeek column 186 px, enough for a five-glyph amount and its
    // currency at the full figure height.
    const int mid = portrait_ ? right : 188;
    c_.dotted(kLeft, rule, c_.width() - 2 * kLeft);

    const auto &s = v.model.snapshot(provider_of(1));
    const bool sp = short_present(s, v.link.encrypted), wp = week_present(s, v.link.encrypted);
    const Paces pace = paces(v, 1, sp, wp);
    c_.text(kLeft, label, kNames[1], 2);
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

    // The balance sits in the right column in landscape and below the bars in portrait,
    // where the page is too narrow for two columns.
    const auto &d = v.model.snapshot(provider_of(0));
    if (portrait_) {
        constexpr int kBalanceMaxH = 44, kBalanceBottom = 356;
        const int balance_x = kLeft + 104;
        const int h = balance_height(d, right - balance_x, kBalanceMaxH);
        // The label sits level with the middle of the balance figure beside it.
        c_.text(kLeft, kBalanceBottom - h + (h - kLine) / 2, kNames[0], 2);
        draw_balance(c_, balance_x, kBalanceBottom - h, d, right - balance_x, kBalanceMaxH);
    } else {
        // The column is wide enough for a five-glyph amount and its currency at the full
        // figure height, and the figure hangs from a fixed top so that a shorter amount
        // keeps its top level with the MiniMax bars.
        constexpr int kBalanceMaxH = 52, kBalanceTop = 209;
        const int column = mid + 12;
        c_.text(column, label, kNames[0], 2);
        draw_balance(c_, column, kBalanceTop, d, right - column, kBalanceMaxH);
    }
}

void Screens::music(const View &v) {
    header(v, "MUSIC", ring_index(Page::Music));
    const int right = c_.width() - kLeft;
    const int content = right - kLeft;
    if (!v.music_valid) {
        c_.text(kLeft, kContentTop, v.link.encrypted ? "WAITING FOR DATA" : "NO MUSIC", 2);
        return;
    }
    const auto &m = v.music;
    char fitted[64];
    // The song and its artist sit above the playhead, in the line height of the lyric font.
    CjkText::fit(fitted, sizeof(fitted), m.title, content, kMusicScale);
    CjkText::draw(c_, kLeft, 46, fitted, kMusicScale);
    if (m.artist[0]) {
        CjkText::fit(fitted, sizeof(fitted), m.artist, content - kPausedWidth, kMusicScale);
        CjkText::draw(c_, kLeft, 74, fitted, kMusicScale);
    }
    // The paused state sits at the right end of the artist row, clear of the title above it.
    if (!m.playing)
        c_.text_right(right, 84, "PAUSED", 2);
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
    for (int row = 0; row < kMusicLyricRows; ++row) {
        const int index = m.current_line + row;
        if (index >= m.line_count)
            break;
        CjkText::fit(fitted, sizeof(fitted), m.lines[index], lyric_width, kMusicScale);
        const int top = kMusicLyricsTop + row * kMusicLyricPitch;
        if (row == 0) {
            // The line being sung is inverted, like the chips elsewhere on the panel.
            c_.fill(kLeft - kMusicTextInset, top - 2, content + 2 * kMusicTextInset,
                    kMusicLineHeight + 4);
            CjkText::draw(c_, kLeft, top, fitted, kMusicScale, Ink::White);
        } else {
            CjkText::draw(c_, kLeft, top, fitted, kMusicScale);
        }
    }
}

void Screens::settings(const View &v) {
    header(v, "SETTINGS");
    struct Row {
        const char *label;
        const char *options[4];
    };
    static const Row rows[kSettingRows] = {
        {"AUTO CYCLE", {"OFF", "30S", "1M", "5M"}},
        {"ALERT 95%", {"OFF", "PAGE", "+BEEP"}},
        {"SECONDS", {"ON", "OFF"}},
        {"ROTATION", {"LAND", "PORT", "FLIP"}},
    };
    const auto &s = v.settings;
    const int right = c_.width() - kLeft;
    const int selected[kSettingRows] = {s.cycle, s.alert, s.seconds ? 0 : 1, s.rotation};
    for (int r = 0; r < kSettingRows; ++r) {
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
        c_.text(kLeft, rule + 10, v.save_error ? "SAVE FAILED" : "KEY NEXT", 2);
        c_.text_right(right, rule + 10, v.save_error ? "" : "BOOT SET", 2);
        c_.text(kLeft, rule + 30, v.save_error ? "BOOT RETRY" : "HOLD EXIT", 2);
    } else if (v.save_error) {
        c_.text(kLeft, rule + 10, "SAVE FAILED / BOOT RETRY", 2);
    } else {
        c_.text(kLeft, rule + 10, "KEY NEXT", 2);
        c_.text((c_.width() - Canvas::text_width("BOOT SET", 2)) / 2, rule + 10, "BOOT SET", 2);
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
        c_.text(kLeft, kPortraitFooterRule + 30, "KEY NEW CODE", 2);
        return;
    }
    c_.fill(kLeft, kFooterRule, right - kLeft, 2);
    c_.text(kLeft, kFooterRule + 10, v.device_name, 2);
    c_.text_right(right, kFooterRule + 10, "KEY NEW CODE", 2);
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
        c_.text(kLeft, kPortraitFooterRule + 10, confirming ? "KEY CONFIRM" : "DO NOT UNPLUG", 2);
        c_.text(kLeft, kPortraitFooterRule + 30, confirming ? "HOLD DENY" : "OR POWER OFF", 2);
        return;
    }
    c_.fill(kLeft, kFooterRule, right - kLeft, 2);
    c_.text(kLeft, kFooterRule + 10,
            confirming ? "KEY CONFIRM / HOLD DENY" : "DO NOT UNPLUG OR POWER OFF", 2);
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
            portrait_ ? home_portrait(v) : focus(v, 0);
            break;
        case Page::Minimax:
            portrait_ ? home_portrait(v) : focus(v, 1);
            break;
        case Page::Music:
            portrait_ ? home_portrait(v) : music(v);
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
