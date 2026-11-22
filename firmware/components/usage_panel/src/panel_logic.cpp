#include "usage_panel/panel_logic.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace usage_panel {
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
    constexpr int options[kSettingRows] = {4, 3, 2, 3, kBrightnessLevelCount};
    return row >= 0 && row < kSettingRows ? options[row] : 0;
}

int brightness_level_index(uint8_t percent) {
    // The Bridge may hold any percent; the Settings page edits the nearest step.
    int best = 0;
    for (int i = 1; i < kBrightnessLevelCount; ++i) {
        const int here = std::abs(static_cast<int>(kBrightnessLevels[i]) - percent);
        const int there = std::abs(static_cast<int>(kBrightnessLevels[best]) - percent);
        if (here < there)
            best = i;
    }
    return best;
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
// A scroll that ends exactly when the line does is never seen to end: frames are composed
// every 250 ms, so the last one before the switch is up to a frame short of the tail. Finish
// this much earlier to leave the final frame on screen at least once.
constexpr uint32_t kLyricEndMarginMs = 400;

/** Columns of text in `width` pixels. */
int text_columns(int width) { return (width + kLyricScrollColumn - 1) / kLyricScrollColumn; }
} // namespace

void apply_sessions(View &view, const SessionsUpdate &update) {
    view.sessions = update;
    view.sessions_valid = true;
}

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
    // The playhead is anchored on the moment the publication arrived; the panel advances it
    // with its own clock until the next publication says where the song is.
    view.music_position_at_ms = now_ms;
    if (!same_line) {
        view.music_line_ms = now_ms;
        // Measured once per line: a position that jitters cannot change the pace mid-line.
        view.music_line_budget_ms =
            update.line_end_seconds > update.position_seconds
                ? (update.line_end_seconds - update.position_seconds) * 1000u
                : 0;
    }
}

namespace {

/**
 * How long the panel keeps advancing the playhead after the last publication.
 *
 * The Bridge publishes once a second, so a gap longer than this is not a slow
 * song: the link or the Bridge stopped. Carrying on with the panel clock would
 * walk the lyrics past the song for as long as the silence lasts, which reads as
 * a jump of many seconds once the frames return, so the panel holds the line it
 * last heard instead. One missed publication stays inside the window.
 */
constexpr uint64_t kMusicStaleMs = 3000;

/** Whether the source stated the start of every line of the window. */
bool has_timeline(const View &view) {
    return view.music.line_time_count != 0 &&
           view.music.line_time_count == view.music.line_count;
}

}  // namespace

uint64_t music_position_ms(const View &view, uint64_t now_ms) {
    if (!view.music.has_position)
        return 0;
    uint64_t position = view.music.position_ms;
    // The song keeps running between two publications, so the playhead does too - but only
    // as far as the next publication would have carried it.
    if (view.music.playing && now_ms > view.music_position_at_ms) {
        const uint64_t since = now_ms - view.music_position_at_ms;
        position += since < kMusicStaleMs ? since : kMusicStaleMs;
    }
    return position;
}

uint8_t music_current_line(const View &view, uint64_t now_ms) {
    const auto &music = view.music;
    if (!has_timeline(view))
        return music.current_line;
    const uint64_t position = music_position_ms(view, now_ms);
    uint8_t current = music.current_line;
    for (uint8_t index = 0; index < music.line_time_count; ++index) {
        if (music.line_times_ms[index] <= position)
            current = index;
        else
            break;
    }
    return current;
}

uint64_t music_line_age_ms(const View &view, uint64_t now_ms) {
    if (!has_timeline(view)) {
        // Without a timeline the panel counts from the moment the line appeared.
        return now_ms > view.music_line_ms ? now_ms - view.music_line_ms : 0;
    }
    const uint8_t current = music_current_line(view, now_ms);
    if (current >= view.music.line_time_count)
        return 0;
    const uint64_t position = music_position_ms(view, now_ms);
    const uint64_t start = view.music.line_times_ms[current];
    return position > start ? position - start : 0;
}

uint32_t music_line_budget_ms(const View &view, uint64_t now_ms) {
    if (!has_timeline(view))
        return view.music_line_budget_ms;
    const uint8_t current = music_current_line(view, now_ms);
    if (current + 1 >= view.music.line_time_count)
        // The last line of the window has no successor, so its length is unknown.
        return 0;
    return view.music.line_times_ms[current + 1] - view.music.line_times_ms[current];
}

bool music_line_moved(const View &view, uint64_t now_ms) {
    return view.music_valid && music_current_line(view, now_ms) != view.music_line_shown;
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
        // pace, so it cannot crawl across the gap. Whatever is left goes to the motion first,
        // because a tight line needs it to arrive rather than to wait.
        const uint32_t usable = budget_ms > kLyricEndMarginMs ? budget_ms - kLyricEndMarginMs : 0;
        lead = std::min<uint32_t>(kLyricLeadInMs, usable / 3);
        const uint32_t room = usable > lead ? usable - lead : 0;
        motion = std::min(motion, room);
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

int ring_index(const View &view, Page page) {
    switch (page) {
    case Page::Home:
        return 0;
    case Page::Deepseek:
        return 1;
    case Page::Minimax:
        return 2;
    case Page::Sessions:
        return view.sessions_valid ? 3 : -1;
    case Page::Music:
        return view.sessions_valid ? 4 : 3;
    case Page::Clock:
        return view.sessions_valid ? 5 : 4;
    default:
        return -1;
    }
}

int ring_pages(const View &view) { return view.sessions_valid ? 6 : 5; }

/**
 * Next ring page from `page`, skipping the sessions page while no publication
 * has arrived. The skip keeps a panel whose Bridge sends no sessions out of an
 * empty page, wherever in the ring the step starts.
 */
static Page next_ring_page(Page page, bool sessions_valid) {
    switch (page) {
    case Page::Home:
        return Page::Deepseek;
    case Page::Deepseek:
        return Page::Minimax;
    case Page::Minimax:
        return sessions_valid ? Page::Sessions : Page::Music;
    case Page::Sessions:
        return Page::Music;
    case Page::Music:
        return Page::Clock;
    default:
        return Page::Home;
    }
}

Page Navigator::next(const View &v) { return next_ring_page(v.page, v.sessions_valid); }

Page Navigator::previous(const View &v) {
    Page p = v.page;
    for (int i = 0; i < ring_pages(v); ++i) {
        if (next_ring_page(p, v.sessions_valid) == v.page)
            return p;
        p = next_ring_page(p, v.sessions_valid);
    }
    return Page::Home;
}

NavResult Navigator::input(Input in, View &v, uint64_t now_ms) {
    NavResult r;
    if (in == Input::None)
        return r;
    last_step_ms_ = now_ms;
    r.redraw = true;
    if (v.alert >= 0 && v.page != Page::Settings) {
        // The alert covers the panel, so a key dismisses it instead of changing the page.
        if (in != Input::Hold) {
            dismissed_ = true;
            v.alert = -1;
        }
        return r;
    }
    if (v.page == Page::Settings) {
        if (in == Input::Hold) {
            v.page = return_page_;
        } else if (in == Input::Home) {
            // The home action moves the focus to the next row this panel offers.
            v.focus = (v.focus + 1) % v.setting_rows;
        } else if (in == Input::Forward || in == Input::Back) {
            if (v.save_error) {
                // Retries the rejected value; the footer shows RETRY in this state.
                r.save = true;
            } else {
                auto &s = v.settings;
                uint8_t *values[kSettingRows] = {&s.cycle, &s.alert, &s.seconds, &s.rotation,
                                                 &s.brightness};
                const int options = setting_options(v.focus);
                // SECONDS lists ON before OFF, so its option index is the inverse of the flag;
                // BRIGHTNESS stores a percent the Bridge may have set to any value, so it
                // steps from the nearest level instead of from a stored index.
                int index;
                if (v.focus == 2)
                    index = s.seconds ? 0 : 1;
                else if (v.focus == 4)
                    index = brightness_level_index(s.brightness);
                else
                    index = *values[v.focus];
                index = (index + (in == Input::Forward ? 1 : options - 1)) % options;
                if (v.focus == 2)
                    s.seconds = index == 0 ? 1 : 0;
                else if (v.focus == 4)
                    s.brightness = kBrightnessLevels[index];
                else
                    *values[v.focus] = static_cast<uint8_t>(index);
                r.save = true;
            }
        }
        return r;
    }
    if (in == Input::Hold) {
        return_page_ = v.page;
        v.page = Page::Settings;
        v.focus = 0;
    } else if (in == Input::Forward)
        v.page = next(v);
    else if (in == Input::Back)
        v.page = previous(v);
    else if (in == Input::Home)
        v.page = Page::Home;
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
    v.page = next(v);
    return true;
}
} // namespace usage_panel
