#pragma once

#include <cstdint>

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "usage_core/sessions.hpp"

namespace usage_panel {

/** Result of decoding one sessions command. */
struct SessionsParseResult {
    bool valid = false;
    SessionsUpdate update{};
};

/**
 * Parses the sessions command of the panel protocol.
 *
 * The command carries the live sessions and their states as two parallel
 * newline-joined strings, plus the recent titles as a third one. Text is
 * copied in whole UTF-8 characters, and the lists must agree in length, so a
 * frame whose states do not address its names is answered with
 * `invalid_request` instead of reaching the display.
 */
SessionsParseResult parse_sessions_command(
    const esp_desktop_buddy_command_view_t* request);

}  // namespace usage_panel
