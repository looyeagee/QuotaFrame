#include "usage_protocol/now_playing_command.hpp"

#include <cstring>

#include "usage_protocol/usage_command.hpp"

namespace usage_panel {
namespace {

/**
 * Copies `text` into `out` without splitting a UTF-8 character.
 *
 * A truncated character would break the decoder of the display, so the copy
 * stops at the last complete character that fits.
 */
void copy_truncated(char* out, size_t out_size, const char* text) {
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!text)
        return;
    size_t length = 0;
    while (text[length] && length + 1 < out_size)
        ++length;
    // Walk back to the start of the character the cut landed in.
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
        --length;
    memcpy(out, text, length);
    out[length] = '\0';
}

/**
 * Parses one decimal number in the canonical text form the protocol uses.
 *
 * Leading zeros, signs, non-digits and values above UINT32_MAX are rejected,
 * like every other decimal field of the protocol.
 */
bool parse_decimal_u32(const char* text, size_t length, uint32_t& value) {
    if (length == 0 || (text[0] == '0' && length > 1))
        return false;
    uint64_t parsed = 0;
    for (size_t index = 0; index < length; ++index) {
        if (text[index] < '0' || text[index] > '9')
            return false;
        parsed = parsed * 10u + static_cast<unsigned>(text[index] - '0');
        if (parsed > UINT32_MAX)
            return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

}  // namespace

NowPlayingParseResult parse_now_playing_command(
    const esp_desktop_buddy_command_view_t* request)
{
    NowPlayingParseResult result{};
    uint32_t version = 0;
    if (get_decimal_u32(request, "seq", result.update.sequence) != FieldResult::Valid ||
        get_decimal_u32(request, "v", version) != FieldResult::Valid || version != 1 ||
        get_decimal_u32(request, "position", result.update.position_seconds) !=
            FieldResult::Valid ||
        get_decimal_u32(request, "duration", result.update.duration_seconds) !=
            FieldResult::Valid)
        return result;

    const char* state = nullptr;
    if (esp_desktop_buddy_command_view_get_string(request, "state", &state) != ESP_OK ||
        !state)
        return result;
    if (strcmp(state, "playing") == 0)
        result.update.playing = true;
    else if (strcmp(state, "paused") != 0)
        return result;

    const char* title = nullptr;
    if (esp_desktop_buddy_command_view_get_string(request, "title", &title) == ESP_OK &&
        title)
        copy_truncated(result.update.title, sizeof(result.update.title), title);
    const char* artist = nullptr;
    if (esp_desktop_buddy_command_view_get_string(request, "artist", &artist) == ESP_OK &&
        artist)
        copy_truncated(result.update.artist, sizeof(result.update.artist), artist);

    const char* lines = nullptr;
    if (esp_desktop_buddy_command_view_get_string(request, "lines", &lines) == ESP_OK &&
        lines) {
        const char* cursor = lines;
        while (*cursor && result.update.line_count < kMaxLyricLines) {
            const char* end = strchr(cursor, '\n');
            const size_t length = end ? static_cast<size_t>(end - cursor)
                                      : strlen(cursor);
            char line[kMaxLyricLineBytes + 1];
            const size_t copied = length < kMaxLyricLineBytes ? length : kMaxLyricLineBytes;
            memcpy(line, cursor, copied);
            line[copied] = '\0';
            copy_truncated(result.update.lines[result.update.line_count],
                           sizeof(result.update.lines[0]), line);
            ++result.update.line_count;
            if (!end)
                break;
            cursor = end + 1;
        }
    }

    // The start of every line of the window, in milliseconds, in the order of the lines.
    const char* line_times = nullptr;
    if (esp_desktop_buddy_command_view_get_string(request, "line_times", &line_times) == ESP_OK &&
        line_times) {
        const char* cursor = line_times;
        while (*cursor) {
            const char* end = strchr(cursor, '\n');
            const size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
            uint32_t value = 0;
            if (!parse_decimal_u32(cursor, length, value))
                return result;
            if (result.update.line_time_count >= kMaxLyricLines)
                return result;
            result.update.line_times_ms[result.update.line_time_count++] = value;
            if (!end)
                break;
            cursor = end + 1;
        }
    }
    // The timestamps address the lines one by one, so a list of another length
    // does not describe this window.
    if (result.update.line_time_count != 0 &&
        result.update.line_time_count != result.update.line_count)
        return result;

    // Optional upper bound for the scroll of the line being sung.
    uint32_t line_end = 0;
    const auto line_end_result = get_decimal_u32(request, "line_end", line_end);
    if (line_end_result == FieldResult::Invalid)
        return result;
    if (line_end_result == FieldResult::Valid)
        result.update.line_end_seconds = line_end;

    // The playhead in milliseconds; a source that states none leaves it at zero. The song
    // may start at zero, so the field's presence is what says the playhead is known.
    uint32_t position_ms = 0;
    const auto position_result = get_decimal_u32(request, "position_ms", position_ms);
    if (position_result == FieldResult::Invalid)
        return result;
    if (position_result == FieldResult::Valid) {
        result.update.position_ms = position_ms;
        result.update.has_position = true;
    }

    uint32_t current = 0;
    const auto current_result = get_decimal_u32(request, "current", current);
    if (current_result == FieldResult::Invalid)
        return result;
    if (current_result == FieldResult::Valid) {
        result.update.current_line =
            current < result.update.line_count ? static_cast<uint8_t>(current) : 0;
    }
    result.valid = true;
    return result;
}

}  // namespace usage_panel
