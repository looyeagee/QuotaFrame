#pragma once

#include <cstdint>

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "usage_core/now_playing.hpp"

namespace usage_panel {

/** Result of decoding one now-playing command. */
struct NowPlayingParseResult {
    bool valid = false;
    NowPlayingUpdate update{};
};

/**
 * Parses the now-playing command of the panel protocol.
 *
 * The command names the song, its artist, the playhead and the state of the
 * player, and may carry a window of lyric lines separated by newlines. Text is
 * copied in whole UTF-8 characters, so a line never ends in a broken
 * character, and the number of lines and their length are bounded.
 */
NowPlayingParseResult parse_now_playing_command(
    const esp_desktop_buddy_command_view_t* request);

}  // namespace usage_panel
