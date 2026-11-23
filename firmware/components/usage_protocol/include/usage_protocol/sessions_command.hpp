#pragma once

#include <cstdint>

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "usage_core/sessions.hpp"

namespace usage_panel {

/** Result of decoding one sessions command. */
struct SessionsParseResult {
    bool valid = false;
};

/**
 * Parses the sessions command of the panel protocol into `out`.
 *
 * The command carries the live sessions and their states as two parallel
 * newline-joined strings, plus the recent titles as a third one. Text is
 * copied in whole UTF-8 characters, and the lists must agree in length, so a
 * frame whose states do not address its names is answered with
 * `invalid_request` instead of reaching the display. A roster longer than the
 * panel keeps is cut at its tail, which leaves the sessions that moved least.
 *
 * A state is `busy`, `idle`, `shell`, or `waiting` with the reason it waits
 * for as `waiting/<reason>`. A frame whose state names neither is rejected,
 * and the reason travels inside the state word, so a roster cannot claim that
 * one session waits for something another one waits for.
 *
 * The caller owns the buffer: the roster is large enough that it does not fit
 * on the stack of the task that receives the frame.
 */
SessionsParseResult parse_sessions_command(
    const esp_desktop_buddy_command_view_t* request, SessionsUpdate& out);

}  // namespace usage_panel
