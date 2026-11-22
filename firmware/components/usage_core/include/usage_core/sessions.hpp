#pragma once

#include <cstdint>

namespace usage_panel {

/** Number of live sessions one publication carries. */
constexpr uint8_t kMaxLiveSessions = 4;
/** Number of recent titles one publication carries. */
constexpr uint8_t kMaxRecentSessions = 3;
/**
 * Bytes of one session name or recent title, without their terminators.
 *
 * A line of Chinese is three bytes a character, so this holds thirty-two of
 * them; the panel's name column shows about twenty-five at a time and cuts
 * the rest at a whole character, matching the truncation the Bridge applied.
 */
constexpr uint16_t kMaxSessionTextBytes = 96;

/** Whether a live session is working or waiting for its user. */
enum class SessionState : uint8_t { Idle, Busy };

struct SessionLive {
    char name[kMaxSessionTextBytes + 1]{};
    SessionState state = SessionState::Idle;
};

struct SessionRecent {
    char title[kMaxSessionTextBytes + 1]{};
};

/**
 * One sessions publication.
 *
 * The live rows are what the pages draw. The recent titles are still parsed because the
 * protocol carries them and an older Bridge sends them, but no page draws them: a panel
 * answers what is running now, and the history of what ran before it belongs to the
 * Bridge's own control page.
 */
struct SessionsUpdate {
    uint32_t sequence = 0;
    uint8_t live_count = 0;
    uint8_t recent_count = 0;
    SessionLive live[kMaxLiveSessions]{};
    SessionRecent recent[kMaxRecentSessions]{};
};

}  // namespace usage_panel
