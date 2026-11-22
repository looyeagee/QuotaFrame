#include "usage_panel/screen_parts.hpp"

#include <cstdio>

namespace usage_panel {

bool balance_present(const ProviderSnapshot &s, bool encrypted) {
    return s.has_valid_data && s.has_balance && (s.latest_balance_present || !encrypted);
}

void format_balance(uint32_t cents, char *out, size_t size) {
    if (cents >= 100000)
        snprintf(out, size, "%lu", static_cast<unsigned long>((cents + 50) / 100));
    else
        snprintf(out, size, "%lu.%02lu", static_cast<unsigned long>(cents / 100),
                 static_cast<unsigned long>(cents % 100));
}

void draw_inline_balance(CanvasSurface &c, int x, int top, const ProviderSnapshot &s, int max_width) {
    char amount[24];
    format_balance(s.balance_cents, amount, sizeof(amount));
    const int width = c.text(x, top, amount, 2);
    const int currency_x = x + width + 6;
    if (width + 6 + CanvasSurface::text_width(s.currency, 2) <= max_width)
        c.text(currency_x, top, s.currency, 2);
    else
        c.text(currency_x, top + kLabelLineHeight - 7, s.currency, 1);
}

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

long long compact_balance(const View &v, int slot) {
    const auto &s = v.model.snapshot(provider_for(slot));
    return balance_present(s, v.link.encrypted) ? static_cast<long long>(s.balance_cents) : -1;
}

const char *empty_message(const View &v) {
    return v.link.encrypted ? "WAITING FOR DATA" : "NO DATA";
}

void reset_countdown(const View &v, int slot, bool week, char *out, size_t size) {
    const auto provider = provider_for(slot);
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

bool age_label(const View &v, int slot, char *out, size_t size) {
    const auto &s = v.model.snapshot(provider_for(slot));
    if (!s.has_valid_data) {
        snprintf(out, size, "NO DATA");
        return true;
    }
    const uint32_t epoch = v.model.estimated_epoch(provider_for(slot), v.now_ms);
    const uint32_t age = epoch > s.sampled_at ? epoch - s.sampled_at : 0;
    format_age(age, out, size);
    return age >= 60;
}

Paces paces(const View &v, int slot, bool sp, bool wp) {
    const auto p = provider_for(slot);
    const auto &s = v.model.snapshot(p);
    const bool enc = v.link.encrypted;
    const uint32_t epoch = v.model.estimated_epoch(p, v.now_ms);
    return {enc && sp ? compute_pace(s.short_window, epoch, kShortWindowSeconds) : Pace{},
            enc && wp ? compute_pace(s.week_window, epoch, kWeekWindowSeconds) : Pace{}};
}

float marker(const Pace &pace) { return pace.valid ? pace_fraction(pace) : -1.f; }

Digits digits_for(int h) { return {h * 5 / 9, std::max(3, h / 10)}; }

int percent_width(int h) { return digits_for(h).w * 3 / 8 + digits_for(h).t; }

int percent_gap(int h) { return digits_for(h).w / 3; }

void percent_sign(CanvasSurface &c, int x, int bottom, int h, Ink ink) {
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

void percent_digits(int percent, char *out, size_t size) {
    if (percent < 0)
        snprintf(out, size, "h--");
    else if (percent >= 100)
        snprintf(out, size, "H00");
    else
        snprintf(out, size, "h%2d", percent);
}

const char *link_status(const View &v) {
    if (v.link.has_passkey)
        return "PAIRING";
    if (v.ota.stage != OtaStage::Idle)
        return "UPDATING";
    return v.link.encrypted ? "LINKED" : "OFFLINE";
}

} // namespace usage_panel
