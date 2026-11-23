#pragma once
#include "usage_panel/view.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
namespace usage_panel {
/**
 * Window lengths used for pace markers. usage.v1 publishes only reset times, so the
 * lengths are fixed: MiniMax resets its five-hour window four times a day and its
 * weekly window every seven days.
 */
constexpr uint32_t kShortWindowSeconds = 5 * 3600;
constexpr uint32_t kWeekWindowSeconds = 7 * 86400;

/** Usage compared with the fraction of the window that has elapsed. */
struct Pace {
    bool valid = false;
    uint32_t elapsed_s = 0;
    uint32_t window_s = 0;
    int delta = 0; // Used percent minus elapsed percent; positive is over pace.
};

/**
 * Computes the pace of one window at `now_epoch`.
 * Invalid when the window has no reset time, the reset has passed, or the remaining
 * time exceeds `window_s` (the fixed length does not match the provider's window).
 */
Pace compute_pace(const UsageWindow &window, uint32_t now_epoch, uint32_t window_s);

/** Elapsed fraction of the window in [0, 1]; 0 for an invalid pace. */
float pace_fraction(const Pace &pace);

/** Short-window countdown "H:MM:SS" (or "H:MM" without seconds); "WAIT" once due; "--" without a reset. */
void format_short_countdown(const UsageWindow &window, uint32_t now_epoch, bool seconds, char *out,
                            size_t size);

/** Week countdown "5D 02:03"; "WAIT" once due; "--" without a reset. */
void format_week_countdown(const UsageWindow &window, uint32_t now_epoch, char *out, size_t size);

/** Elapsed duration as "3H36M", or "2D 04H" from one day. */
void format_elapsed(uint32_t seconds, char *out, size_t size);

/** Sample age: "NOW" below a minute, then "12M AGO", then "3H AGO" from 100 minutes. */
void format_age(uint32_t seconds, char *out, size_t size);

/**
 * Warning level with hysteresis: 2 from 95 % (held down to 93 %), 1 from 80 % (held down
 * to 78 %), otherwise 0.
 */
constexpr uint8_t warning_level(uint8_t previous, uint8_t used) {
    if (used >= 95 || (previous == 2 && used >= 93))
        return 2;
    if (used >= 80 || (previous >= 1 && used >= 78))
        return 1;
    return 0;
}

/** Provider rendered in one slot: DeepSeek holds the balance, MiniMax the windows. */
Provider provider_for(int slot);

/**
 * Applies a sessions publication to the view. The publication replaces the
 * previous roster whole; the first one is what makes the sessions page appear
 * in the ring.
 */
void apply_sessions(View &view, const SessionsUpdate &update);

/**
 * Applies a now-playing publication and restarts the lyric scroll when the line being sung
 * changes. The scroll anchor is the moment the panel began showing that line, so a line that
 * has to move starts at its beginning instead of wherever an earlier line left off.
 *
 * The publication also anchors the playhead: the panel advances it with its own clock until
 * the next publication arrives, so the line being sung follows the song instead of the
 * publication cadence.
 */
void apply_now_playing(View &view, const NowPlayingUpdate &update, uint64_t now_ms);

/**
 * Playhead in milliseconds, advanced by the panel clock while the song plays.
 *
 * Returns 0 when the source states no position. The value is the source's playhead plus the
 * time that passed since the publication arrived, so it lags the song by the time the
 * publication spent on the link.
 */
uint64_t music_position_ms(const View &view, uint64_t now_ms);

/**
 * Index of the line being sung inside the window.
 *
 * The source states the start of every line, so the panel moves on to the next line itself
 * when the playhead reaches it instead of waiting for the next publication. The source's own
 * index is the floor: a panel that is ahead of the publication does not fall back behind it.
 */
uint8_t music_current_line(const View &view, uint64_t now_ms);

/**
 * Milliseconds since the line being sung began, on the song's own timeline.
 *
 * Without a timeline the panel counts from the moment the line appeared, which is as precise
 * as the publication that carried it.
 */
uint64_t music_line_age_ms(const View &view, uint64_t now_ms);

/**
 * Milliseconds the line being sung lasts, as the source states it.
 *
 * It is an upper bound for the scroll: the scroll may finish sooner, never later. Returns 0
 * when the source cannot say how long the line lasts.
 */
uint32_t music_line_budget_ms(const View &view, uint64_t now_ms);

/**
 * Whether the song has moved on to a line the panel has not drawn yet.
 *
 * The panel advances the playhead itself, so this is what makes it compose a frame at the
 * moment the next line is sung rather than at the next quarter-second boundary.
 */
bool music_line_moved(const View &view, uint64_t now_ms);

/**
 * Right-to-left offset for a `line_width` line with `travel` pixels of overflow, `age_ms`
 * after it appeared.
 *
 * The pace comes from the text: `900 ms` to read its first word plus `100 ms` per column of
 * the whole line, so a narrow row scrolls faster instead of longer. `budget_ms` is how long
 * the source says the line still has, or 0 when it cannot say; it may shorten the scroll to
 * fit, never stretch it, which is why a line followed by an instrumental interlude keeps the
 * content pace and cannot crawl across the gap. Offsets are whole columns, so a moving line
 * never shows half a glyph, and a line with no overflow returns zero.
 */
int lyric_scroll_offset(int line_width, int travel, uint64_t age_ms, uint32_t budget_ms);

/** Text column the lyric scroll steps by: the ASCII cell of the lyric font at its scale. */
constexpr int kLyricScrollColumn = 12;

/**
 * Header ring index of a page, or -1 when the page is outside the ring.
 *
 * The sessions page only sits in the ring once a publication has arrived, so
 * the index of the pages after it depends on the view.
 */
int ring_index(const View &view, Page page);

/** Number of pages in the header ring. */
int ring_pages(const View &view);

/** Whether the short window of a provider has a value that may be displayed. */
bool short_present(const ProviderSnapshot &s, bool encrypted);
/** Whether the week window of a provider has a value that may be displayed. */
bool week_present(const ProviderSnapshot &s, bool encrypted);

/**
 * Panel input.
 *
 * Back and Forward are the two keys that step through the page ring. Home is the middle
 * action: the chord of both keys on a board that has two, a key of its own on a board that
 * has three. Hold is any key held for 800 ms.
 */
enum class Input : uint8_t { None, Back, Forward, Home, Hold };

/**
 * One input from the panel.
 *
 * `action` is the gesture the panel recognised; `scroll_px` is the distance a vertical
 * drag covered since the last poll, in logical pixels. The two travel together: a drag
 * scrolls the list of a page that is longer than the glass, and the list follows the
 * finger pixel by pixel instead of stepping a row at a time.
 */
struct PanelInput {
    Input action = Input::None;
    int scroll_px = 0; // Pixels the list moves down; negative moves it up.
};

/** Effects of one navigation step that the application loop must carry out. */
struct NavResult {
    bool redraw = false;
    bool save = false; // Settings changed and must be persisted.
};

/** Page ring, alert dismissal and auto cycling. */
class Navigator {
  public:
    /**
     * Next ring page, skipping the sessions page while no publication has
     * arrived. The ring holds Home, Deepseek, Minimax, Sessions, Music and
     * Clock; the sessions page only sits in it for a panel that has data.
     */
    static Page next(const View &view);
    /** Previous ring page with the same skip; Home when outside the ring. */
    static Page previous(const View &view);

    /** Applies one input to `view`. `now_ms` restarts the auto-cycle timer. */
    NavResult input(const PanelInput &input, View &view, uint64_t now_ms);

    /**
     * Updates `view.alert` from the MiniMax warning level, the only service with a
     * quota. A dismissed alert stays hidden until that level drops below 2.
     * @return true on the first presentation at level 2; reconnects do not re-arm it.
     */
    bool update_alert(View &view);

    /** Advances the ring when the configured auto-cycle interval has elapsed. */
    bool auto_cycle(View &view, uint64_t now_ms);

  private:
    Page return_page_ = Page::Home;
    bool dismissed_ = false;
    bool announced_ = false;
    uint64_t last_step_ms_ = 0;
    /** Moves the sessions list by `delta_px` pixels, inside the part of the roster the
     *  glass cannot show. */
    void scroll_sessions(int delta_px, View &view);
};

/** Auto-cycle interval in milliseconds for a Settings::cycle value; 0 disables cycling. */
uint32_t cycle_interval_ms(uint8_t cycle);

/** Number of options on a Settings row. */
int setting_options(int row);

/** Nearest kBrightnessLevels position for a percent, so the Settings page can step from it. */
int brightness_level_index(uint8_t percent);
} // namespace usage_panel
