#pragma once

#include <cstdint>

namespace usage_panel {

/**
 * Number of live sessions one publication carries.
 *
 * The roster is the whole list of running sessions and not a sample of it: the
 * Bridge sends as many as the machine runs, and a panel keeps the ones it can
 * show. The bound keeps the publication inside the frame the link carries.
 */
constexpr uint8_t kMaxLiveSessions = 24;
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

/**
 * How a live session is spending its time.
 *
 * The words are the ones the Bridge sends, and they are the ones Claude Code
 * publishes for a running session. A session that waits is the one that spends
 * the reader's time rather than its own, so it keeps a state of its own.
 */
enum class SessionState : uint8_t { Idle, Busy, Waiting, Shell };

/**
 * What a waiting session waits for.
 *
 * Each reason becomes a short word on the panel. A reason the Bridge does not
 * name yet is still a dialog that holds the session, so it reads as one.
 */
enum class WaitingReason : uint8_t { None, Ask, Permission, Sandbox, Worker, Goal, Dialog };

struct SessionLive {
    char name[kMaxSessionTextBytes + 1]{};
    SessionState state = SessionState::Idle;
    // What the session waits for; only a waiting session states one.
    WaitingReason waiting_for = WaitingReason::None;
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
