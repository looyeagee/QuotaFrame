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
 */
struct NowPlayingUpdate {
    uint32_t sequence = 0;
    uint32_t position_seconds = 0;
    uint32_t duration_seconds = 0;
    bool playing = false;
    uint8_t current_line = 0;
    uint8_t line_count = 0;
    char title[kMaxTitleBytes + 1]{};
    char artist[kMaxTitleBytes + 1]{};
    char lines[kMaxLyricLines][kMaxLyricLineBytes + 1]{};
};

}  // namespace usage_panel
