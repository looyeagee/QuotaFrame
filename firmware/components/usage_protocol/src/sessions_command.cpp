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
        // or a word that is neither busy nor idle, does not describe this roster.
        size_t counted = 0;
        const char* cursor = states;
        while (*cursor) {
            const char* end = strchr(cursor, '\n');
            const size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
            if (counted >= names)
                return result;
            if (length == 4 && memcmp(cursor, "busy", 4) == 0)
                result.update.live[counted].state = SessionState::Busy;
            else if (length == 4 && memcmp(cursor, "idle", 4) == 0)
                result.update.live[counted].state = SessionState::Idle;
            else
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
