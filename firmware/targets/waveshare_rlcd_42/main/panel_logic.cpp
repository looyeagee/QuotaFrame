#include "panel_logic.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace usage_panel::rlcd {
Pace compute_pace(const UsageWindow &window, uint32_t now_epoch, uint32_t window_s) {
    Pace pace;
    if (!window.present || !window.has_reset || !now_epoch || window.reset_at <= now_epoch)
        return pace;
    const uint32_t remaining = window.reset_at - now_epoch;
    if (remaining > window_s)
        return pace;
    pace.valid = true;
    pace.window_s = window_s;
    pace.elapsed_s = window_s - remaining;
    const int elapsed_percent =
        static_cast<int>((static_cast<uint64_t>(pace.elapsed_s) * 100 + window_s / 2) / window_s);
    pace.delta = window.used_percent - elapsed_percent;
    return pace;
}

float pace_fraction(const Pace &pace) {
    return pace.valid ? static_cast<float>(pace.elapsed_s) / pace.window_s : 0.f;
}

namespace {
bool countdown_special(const UsageWindow &window, uint32_t now_epoch, char *out, size_t size) {
    if (!window.has_reset || !now_epoch) {
        snprintf(out, size, "--");
        return true;
    }
    if (window.reset_at <= now_epoch) {
        snprintf(out, size, "WAIT");
        return true;
    }
    return false;
}
} // namespace

void format_short_countdown(const UsageWindow &window, uint32_t now_epoch, bool seconds, char *out,
                            size_t size) {
    if (countdown_special(window, now_epoch, out, size))
        return;
    const uint32_t s = window.reset_at - now_epoch;
    if (seconds)
        snprintf(out, size, "%lu:%02lu:%02lu", static_cast<unsigned long>(s / 3600),
                 static_cast<unsigned long>(s / 60 % 60), static_cast<unsigned long>(s % 60));
    else
        snprintf(out, size, "%lu:%02lu", static_cast<unsigned long>(s / 3600),
                 static_cast<unsigned long>(s / 60 % 60));
}

void format_week_countdown(const UsageWindow &window, uint32_t now_epoch, char *out, size_t size) {
    if (countdown_special(window, now_epoch, out, size))
        return;
    const uint32_t s = window.reset_at - now_epoch;
    snprintf(out, size, "%luD %02lu:%02lu", static_cast<unsigned long>(s / 86400),
             static_cast<unsigned long>(s / 3600 % 24), static_cast<unsigned long>(s / 60 % 60));
}

void format_elapsed(uint32_t seconds, char *out, size_t size) {
    if (seconds >= 86400)
        snprintf(out, size, "%luD %02luH", static_cast<unsigned long>(seconds / 86400),
                 static_cast<unsigned long>(seconds / 3600 % 24));
    else
        snprintf(out, size, "%luH%02luM", static_cast<unsigned long>(seconds / 3600),
                 static_cast<unsigned long>(seconds / 60 % 60));
}

void format_age(uint32_t seconds, char *out, size_t size) {
    if (seconds < 60)
        snprintf(out, size, "NOW");
    else if (seconds < 100 * 60)
        snprintf(out, size, "%luM AGO", static_cast<unsigned long>(seconds / 60));
    else
        snprintf(out, size, "%luH AGO", static_cast<unsigned long>(seconds / 3600));
}

// Offline panels keep showing the last values; online panels show only windows carried by
// the latest publication.
bool short_present(const ProviderSnapshot &s, bool encrypted) {
    return s.has_valid_data && s.short_window.present && (s.latest_short_present || !encrypted);
}
bool week_present(const ProviderSnapshot &s, bool encrypted) {
    return s.has_valid_data && s.week_window.present && (s.latest_week_present || !encrypted);
}

uint32_t cycle_interval_ms(uint8_t cycle) {
    constexpr uint32_t intervals[] = {0, 30000, 60000, 300000};
    return cycle < 4 ? intervals[cycle] : 0;
}

int setting_options(int row) {
    constexpr int options[kSettingRows] = {4, 3, 2, 3};
    return row >= 0 && row < kSettingRows ? options[row] : 0;
}

namespace {
// The line holds still long enough to read its first word before it starts to move.
constexpr uint32_t kLyricLeadInMs = 900;
// Reading the line takes this long plus this much per column of text. The columns come from
// the whole line, not from how far the row has to scroll, so a narrow row scrolls faster
// instead of longer: the same words read in the same time on the music page and on the clock
// page. Without this the clock page's column, two thirds the width, took nearly twice as long
// as the line lasted and never reached its end.
constexpr uint32_t kLyricBaseMs = 900;
constexpr uint32_t kLyricColumnMs = 100;
// Shortest motion for a line whose remaining time is almost gone.
constexpr uint32_t kLyricFloorMs = 600;

/** Columns of text in `width` pixels. */
int text_columns(int width) { return (width + kLyricScrollColumn - 1) / kLyricScrollColumn; }
} // namespace

void apply_now_playing(View &view, const NowPlayingUpdate &update, uint64_t now_ms) {
    // The window the Bridge sends is anchored on the sung line, so comparing that one line is
    // enough to know whether the text under the highlight changed.
    bool same_line = view.music_valid && update.line_count > 0 && view.music.line_count > 0;
    if (same_line) {
        const uint8_t before = view.music.current_line;
        const uint8_t after = update.current_line;
        same_line = before < view.music.line_count && after < update.line_count &&
                    std::strcmp(view.music.lines[before], update.lines[after]) == 0;
    }
    view.music = update;
    view.music_valid = true;
    if (!same_line) {
        view.music_line_ms = now_ms;
        // Measured once per line: a position that jitters cannot change the pace mid-line.
        view.music_line_budget_ms =
            update.line_end_seconds > update.position_seconds
                ? (update.line_end_seconds - update.position_seconds) * 1000u
                : 0;
    }
}

int lyric_scroll_offset(int line_width, int travel, uint64_t age_ms, uint32_t budget_ms) {
    if (travel <= 0)
        return 0;
    // Content pace: what the line itself needs to be read.
    uint32_t motion = kLyricBaseMs + static_cast<uint32_t>(text_columns(line_width)) * kLyricColumnMs;
    uint32_t lead = kLyricLeadInMs;
    if (budget_ms > 0) {
        // The source says how long this line still has. It may shorten the scroll, never
        // stretch it: a line followed by an interlude has minutes left and keeps the content
        // pace, so it cannot crawl across the gap.
        if (budget_ms > lead + kLyricFloorMs) {
            motion = std::min(motion, budget_ms - lead);
        } else {
            lead = 0;
            motion = std::min<uint32_t>(motion, std::max<uint32_t>(budget_ms, kLyricFloorMs));
        }
    }
    if (motion == 0)
        motion = 1;
    if (age_ms < lead)
        return 0;
    const int columns = text_columns(travel);
    const uint64_t moving = age_ms - lead;
    const uint64_t step = moving * static_cast<uint64_t>(columns) / motion;
    return static_cast<int>(std::min<uint64_t>(step, static_cast<uint64_t>(columns))) *
           kLyricScrollColumn;
}

Provider provider_for(int slot) { return slot ? Provider::Minimax : Provider::Deepseek; }

bool service_visible(const View &v, int slot) {
    return v.model.snapshot(provider_for(slot)).has_valid_data;
}

int ring_index(Page page) {
    switch (page) {
    case Page::Home:
        return 0;
    case Page::Deepseek:
        return 1;
    case Page::Minimax:
        return 2;
    case Page::Music:
        return 3;
    case Page::Clock:
        return 4;
    default:
        return -1;
    }
}

int ring_pages() { return 5; }

Page Navigator::next(Page page, bool portrait, bool) {
    if (portrait)
        return page == Page::Home ? Page::Clock : Page::Home;
    switch (page) {
    case Page::Home:
        return Page::Deepseek;
    case Page::Deepseek:
        return Page::Minimax;
    case Page::Minimax:
        return Page::Music;
    case Page::Music:
        return Page::Clock;
    default:
        return Page::Home;
    }
}

Page Navigator::previous(Page page, bool portrait, bool) {
    Page p = page;
    for (int i = 0; i < ring_pages(); ++i) {
        if (next(p, portrait) == page)
            return p;
        p = next(p, portrait);
    }
    return Page::Home;
}

bool Navigator::normalize(View &v) {
    const bool portrait = v.settings.rotation == static_cast<uint8_t>(Rotation::Portrait);
    if (portrait && (v.page == Page::Deepseek || v.page == Page::Minimax ||
                     v.page == Page::Music)) {
        v.page = Page::Home;
        return true;
    }
    return false;
}

NavResult Navigator::input(Input in, View &v, uint64_t now_ms) {
    NavResult r;
    if (in == Input::None)
        return r;
    last_step_ms_ = now_ms;
    r.redraw = true;
    const bool portrait = v.settings.rotation == static_cast<uint8_t>(Rotation::Portrait);
    if (v.alert >= 0 && v.page != Page::Settings) {
        if (in == Input::Key) {
            dismissed_ = true;
            v.alert = -1;
        }
        return r;
    }
    if (v.page == Page::Settings) {
        if (in == Input::Key)
            v.focus = (v.focus + 1) % kSettingRows;
        else if (in == Input::KeyHold) {
            v.page = return_page_;
            normalize(v);
        } else if (v.save_error) {
            // Retries the rejected value; the footer shows BOOT RETRY in this state.
            r.save = true;
        } else {
            auto &s = v.settings;
            uint8_t *values[kSettingRows] = {&s.cycle, &s.alert, &s.seconds, &s.rotation};
            uint8_t &value = *values[v.focus];
            // SECONDS lists ON before OFF, so its option index is the inverse of the flag.
            if (v.focus == 2)
                value = value ? 0 : 1;
            else
                value = static_cast<uint8_t>((value + 1) % setting_options(v.focus));
            r.save = true;
        }
        return r;
    }
    if (in == Input::KeyHold) {
        return_page_ = v.page;
        v.page = Page::Settings;
        v.focus = 0;
    } else if (in == Input::Key)
        v.page = previous(v.page, portrait);
    else
        v.page = next(v.page, portrait);
    return r;
}

bool Navigator::update_alert(View &v) {
    // Only MiniMax publishes a quota window, so only its warning can raise an alert.
    constexpr int kSlot = 1;
    const uint8_t warning = v.warning[kSlot];
    if (warning < 2) {
        dismissed_ = false;
        announced_ = false;
    }
    const bool candidate =
        warning == 2 && !dismissed_ && v.link.encrypted &&
        v.settings.alert != static_cast<uint8_t>(AlertMode::Off);
    const bool raised = candidate && !announced_;
    if (candidate)
        announced_ = true;
    v.alert = candidate ? kSlot : -1;
    return raised;
}

bool Navigator::auto_cycle(View &v, uint64_t now_ms) {
    const uint32_t interval = cycle_interval_ms(v.settings.cycle);
    if (!interval || v.page == Page::Settings || v.alert >= 0) {
        last_step_ms_ = now_ms;
        return false;
    }
    if (now_ms - last_step_ms_ < interval)
        return false;
    last_step_ms_ = now_ms;
    v.page = next(v.page, v.settings.rotation == static_cast<uint8_t>(Rotation::Portrait));
    return true;
}
} // namespace usage_panel::rlcd
