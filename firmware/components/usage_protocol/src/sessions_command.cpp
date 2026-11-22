#include "usage_protocol/sessions_command.hpp"

#include <cstring>

#include "usage_protocol/usage_command.hpp"

namespace usage_panel {
namespace {

/**
 * Copies one newline-separated string into the live-session roster.
 *
 * Every entry is copied in whole UTF-8 characters; a list longer than the
 * roster stops there, which is what the Bridge already bounded.
 * @return the number of entries copied.
 */
size_t copy_live_list(SessionLive (&entries)[kMaxLiveSessions], const char* text) {
    size_t count = 0;
    const char* cursor = text;
    while (*cursor && count < kMaxLiveSessions) {
        const char* end = strchr(cursor, '\n');
        const size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
        char line[kMaxSessionTextBytes + 1];
        const size_t copied = length < kMaxSessionTextBytes ? length : kMaxSessionTextBytes;
        memcpy(line, cursor, copied);
        line[copied] = '\0';
        copy_utf8_truncated(entries[count].name, sizeof(entries[count].name), line);
        ++count;
        if (!end)
            break;
        cursor = end + 1;
    }
    return count;
}

/** Copies one newline-separated string into the recent titles. */
size_t copy_recent_list(SessionRecent (&entries)[kMaxRecentSessions], const char* text) {
    size_t count = 0;
    const char* cursor = text;
    while (*cursor && count < kMaxRecentSessions) {
        const char* end = strchr(cursor, '\n');
        const size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
        char line[kMaxSessionTextBytes + 1];
        const size_t copied = length < kMaxSessionTextBytes ? length : kMaxSessionTextBytes;
        memcpy(line, cursor, copied);
        line[copied] = '\0';
        copy_utf8_truncated(entries[count].title, sizeof(entries[count].title), line);
        ++count;
        if (!end)
            break;
        cursor = end + 1;
    }
    return count;
}

/** The reason a waiting session waits for, from the word the Bridge sends. */
WaitingReason waiting_reason(const char* word, size_t length) {
    if (length == 3 && memcmp(word, "ask", 3) == 0)
        return WaitingReason::Ask;
    if (length == 10 && memcmp(word, "permission", 10) == 0)
        return WaitingReason::Permission;
    if (length == 7 && memcmp(word, "sandbox", 7) == 0)
        return WaitingReason::Sandbox;
    if (length == 6 && memcmp(word, "worker", 6) == 0)
        return WaitingReason::Worker;
    if (length == 4 && memcmp(word, "goal", 4) == 0)
        return WaitingReason::Goal;
    if (length == 6 && memcmp(word, "dialog", 6) == 0)
        return WaitingReason::Dialog;
    return WaitingReason::None;
}

/**
 * Reads one state word of the roster.
 *
 * A word is a state, or a waiting state with the reason it waits for:
 * `busy`, `idle`, `shell`, `waiting` or `waiting/<reason>`. The reason is
 * part of the word, so the two cannot disagree about which session waits for
 * what. A word that names neither is not a state of this protocol, and the
 * frame is rejected.
 * @return whether the word names a state.
 */
bool read_state(SessionLive& entry, const char* word, size_t length) {
    if (length == 4 && memcmp(word, "busy", 4) == 0) {
        entry.state = SessionState::Busy;
        return true;
    }
    if (length == 4 && memcmp(word, "idle", 4) == 0) {
        entry.state = SessionState::Idle;
        return true;
    }
    if (length == 5 && memcmp(word, "shell", 5) == 0) {
        entry.state = SessionState::Shell;
        return true;
    }
    if (length == 7 && memcmp(word, "waiting", 7) == 0) {
        entry.state = SessionState::Waiting;
        entry.waiting_for = WaitingReason::Dialog;
        return true;
    }
    if (length > 8 && memcmp(word, "waiting/", 8) == 0) {
        const WaitingReason reason = waiting_reason(word + 8, length - 8);
        if (reason == WaitingReason::None)
            return false;
        entry.state = SessionState::Waiting;
        entry.waiting_for = reason;
        return true;
    }
    return false;
}

}  // namespace

SessionsParseResult parse_sessions_command(
    const esp_desktop_buddy_command_view_t* request)
{
    SessionsParseResult result{};
    uint32_t version = 0;
    if (get_decimal_u32(request, "seq", result.update.sequence) != FieldResult::Valid ||
        get_decimal_u32(request, "v", version) != FieldResult::Valid || version != 1)
        return result;

    const char* live = nullptr;
    const char* states = nullptr;
    const char* recent = nullptr;
    const bool has_live =
        esp_desktop_buddy_command_view_get_string(request, "live", &live) == ESP_OK && live;
    const bool has_states =
        esp_desktop_buddy_command_view_get_string(request, "states", &states) == ESP_OK &&
        states;
    const bool has_recent =
        esp_desktop_buddy_command_view_get_string(request, "recent", &recent) == ESP_OK && recent;
    // The two lists address each other line by line, so they travel together.
    if (has_live != has_states)
        return result;

    if (has_live && has_states) {
        const size_t names = copy_live_list(result.update.live, live);
        // The state list is matched entry by entry: a shorter or longer one,
        // or a word that names no state, does not describe this roster.
        size_t counted = 0;
        const char* cursor = states;
        while (*cursor) {
            const char* end = strchr(cursor, '\n');
            const size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
            if (counted >= names)
                return result;
            if (!read_state(result.update.live[counted], cursor, length))
                return result;
            ++counted;
            if (!end)
                break;
            cursor = end + 1;
        }
        if (counted != names)
            return result;
        result.update.live_count = static_cast<uint8_t>(names);
    }

    if (has_recent)
        result.update.recent_count =
            static_cast<uint8_t>(copy_recent_list(result.update.recent, recent));

    result.valid = true;
    return result;
}

}  // namespace usage_panel
