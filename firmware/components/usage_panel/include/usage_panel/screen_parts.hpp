#pragma once

#include "usage_panel/canvas.hpp"
#include "usage_panel/panel_logic.hpp"
#include "usage_panel/view.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace usage_panel {

/** The provider names the panels show, in slot order. */
inline constexpr const char *kProviderNames[2] = {"DEEPSEEK", "MINIMAX"};

/** The weekday names the clock page shows, starting at Sunday. */
inline constexpr const char *kDayNames[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

/** Height of one line of the pixel font at the label scale, in pixels. */
constexpr int kLabelLineHeight = 14;

/** Whether the cached balance may be drawn: retained values survive a disconnect. */
bool balance_present(const ProviderSnapshot &s, bool encrypted);

/**
 * Formats a balance in whole cents as "12.34".
 *
 * From 1000 units the decimals are dropped: the segment field for five glyphs does not fit
 * the narrowest content column.
 */
void format_balance(uint32_t cents, char *out, size_t size);

/**
 * Segment height that keeps the amount of `s` and its currency inside `max_width` at the
 * 5:9 cell ratio, never taller than `max_height`.
 *
 * `heights` are the heights the figure tries, largest first; the smallest one is the
 * fallback when none of them fits.
 */
template <size_t N>
int balance_height(const ProviderSnapshot &s, int max_width, int max_height,
                   const int (&heights)[N]) {
    char amount[24];
    format_balance(s.balance_cents, amount, sizeof(amount));
    const int cur_w = CanvasSurface::text_width(s.currency, 2);
    for (int h : heights) {
        if (h > max_height)
            continue;
        const int w = h * 5 / 9;
        // The currency sits 6 px after the amount, so both have to fit.
        if (CanvasSurface::segments_width(amount, w, std::max(3, h / 8)) + 6 + cur_w <= max_width)
            return h;
    }
    return heights[N - 1];
}

/**
 * Draws the balance amount and its currency with the figure top at `top`.
 *
 * @return the figure height, so a caller can align the currency or a label with it.
 */
template <size_t N>
int draw_balance(CanvasSurface &c, int x, int top, const ProviderSnapshot &s, int max_width,
                 int max_height, const int (&heights)[N]) {
    char amount[24];
    format_balance(s.balance_cents, amount, sizeof(amount));
    const int h = balance_height(s, max_width, max_height, heights);
    const int w = h * 5 / 9, t = std::max(3, h / 8);
    const int width = c.segments(x, top, amount, w, h, t);
    const int cur_w = CanvasSurface::text_width(s.currency, 2);
    if (width + 6 + cur_w <= max_width)
        c.text(x + width + 6, top + h - kLabelLineHeight, s.currency, 2);
    return h;
}

/**
 * Draws the balance in the pixel font at (`x`, `top`) with its currency 6 px after the amount.
 *
 * The amount keeps the label's line height, so the provider name and its balance read as one
 * caption row. The currency drops to the small scale when a six-glyph amount would otherwise
 * not fit before `max_width`; it is then bottom-aligned with the label's line box.
 */
void draw_inline_balance(CanvasSurface &c, int x, int top, const ProviderSnapshot &s, int max_width);

/** Playhead as "1:05 / 4:35", or with an hours field for long tracks. */
void format_playhead(uint32_t position, uint32_t duration, char *out, size_t size);

/** Balance in cents for the balance service, or -1 when it has none to show. */
long long compact_balance(const View &v, int slot);

/** Empty-row message when neither window has a value. */
const char *empty_message(const View &v);

/** Offline or absent windows have no live reset countdown. */
void reset_countdown(const View &v, int slot, bool week, char *out, size_t size);

/** Sample age label; false when the sample is under a minute old and needs no label. */
bool age_label(const View &v, int slot, char *out, size_t size);

/** Pace markers for the two windows of one provider; invalid while offline or absent. */
struct Paces {
    Pace short_pace, week_pace;
};
Paces paces(const View &v, int slot, bool sp, bool wp);

/** The fraction of the window that has elapsed, or -1 when the pace is unknown. */
float marker(const Pace &pace);

/** Seven-segment proportions for a given digit height. */
struct Digits {
    int w, t;
};
Digits digits_for(int h);

/** The percent sign is 3/8 of a digit wide plus one stroke, bottom-aligned. */
int percent_width(int h);

/**
 * Space between the digit field and the percent sign: a third of a digit, so a 1 in the last
 * cell (lit on the right-hand side of its cell) does not read as touching the sign.
 */
int percent_gap(int h);

/** Percent sign drawn with the stroke thickness of the digits: two square dots and a diagonal. */
void percent_sign(CanvasSurface &c, int x, int bottom, int h, Ink ink);

/**
 * A fixed field of a hundreds half digit and two digit cells, right-aligned like a segment LCD:
 * 5 shows as a lone digit in the last cell, 100 lights the half digit.
 */
void percent_digits(int percent, char *out, size_t size);

/** Status text for the header: PAIRING, UPDATING, LINKED or OFFLINE. */
const char *link_status(const View &view);

/**
 * The word a panel shows for the state of a running session.
 *
 * The words are the ones the Bridge sends: a session that waits for its
 * reader says so, and one whose background shell still runs says that.
 */
const char *session_state_word(SessionState state);

/**
 * The ink a panel marks the state of a running session with.
 *
 * A session that waits is drawn in the warning ink: it is the one state that
 * spends the reader's time rather than its own.
 */
Ink session_state_ink(SessionState state);

/**
 * The word a panel shows for what a waiting session waits for.
 *
 * Empty when the session states no reason. The words are short, because they
 * share the row with the session's name.
 */
const char *waiting_reason_word(WaitingReason reason);

/** Rounds a float to the nearest whole pixel. */
inline int round_px(float value) { return static_cast<int>(std::lround(value)); }

} // namespace usage_panel
