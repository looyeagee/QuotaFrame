#pragma once

#include <cstdint>

namespace usage_panel {

/** Number of lyric lines one publication carries. */
constexpr uint8_t kMaxLyricLines = 8;
/**
 * Bytes of one lyric line, without its terminator.
 *
 * A line of Chinese is three bytes a character, so this holds sixty-four of them; the panel
 * shows about fifteen at a time and scrolls the rest. Lines longer than this arrive truncated
 * at a whole character, which is what the panel used to do to every line at sixteen.
 */
constexpr uint16_t kMaxLyricLineBytes = 192;
/** Bytes of the song title and of the artist, without their terminators. */
constexpr uint8_t kMaxTitleBytes = 96;

/**
 * One now-playing publication.
 *
 * The lyrics travel as a window of lines around the line being sung, so the
 * panel can show what comes next without holding a whole song in memory.
 *
 * The source states the playhead and the start of every line of the window in
 * milliseconds. A panel places the song on that timeline instead of counting
 * from the moment the publication arrived, so a line is sung and shown at the
 * same instant. The seconds are the same instants rounded down, for a panel
 * that only draws a clock.
 */
struct NowPlayingUpdate {
    uint32_t sequence = 0;
    uint32_t position_seconds = 0;
    uint32_t duration_seconds = 0;
    bool playing = false;
    uint8_t current_line = 0;
    uint8_t line_count = 0;
    /**
     * Position at which this line stops and the next one starts, in seconds; 0 when the
     * source cannot say. Only an upper bound: a panel shortens a scroll to fit inside it and
     * never stretches one to fill it.
     */
    uint32_t line_end_seconds = 0;
    /** Playhead in milliseconds; only meaningful when `has_position` is set. */
    uint32_t position_ms = 0;
    /** Whether the source stated a playhead. */
    bool has_position = false;
    /**
     * Start of every line of the window, in milliseconds, in the order of `lines`.
     * `line_time_count` says how many of them the source stated.
     */
    uint32_t line_times_ms[kMaxLyricLines]{};
    uint8_t line_time_count = 0;
    char title[kMaxTitleBytes + 1]{};
    char artist[kMaxTitleBytes + 1]{};
    char lines[kMaxLyricLines][kMaxLyricLineBytes + 1]{};
};

}  // namespace usage_panel
