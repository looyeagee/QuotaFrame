#include <cstring>

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/transport_port.h"
#include "fixture_data.hpp"
#include "unity.h"
#include "usage_protocol/now_playing_command.hpp"

using namespace usage_panel;

namespace {

struct Capture {
    unsigned accepted = 0;
    NowPlayingUpdate update{};
};

esp_desktop_buddy_command_extension_result_t handle_now_playing(
    void* context,
    esp_desktop_buddy_t*,
    const esp_desktop_buddy_command_view_t* request)
{
    auto* capture = static_cast<Capture*>(context);
    const auto parsed = parse_now_playing_command(request);
    if (parsed.valid) {
        capture->accepted++;
        capture->update = parsed.update;
    }
    esp_desktop_buddy_command_extension_result_t result{};
    result.mode = ESP_DESKTOP_BUDDY_COMMAND_EXTENSION_ACK;
    result.reply.ok = parsed.valid;
    result.reply.n = parsed.update.sequence;
    result.reply.error = parsed.valid ? nullptr : "invalid_request";
    return result;
}

void feed_line(const char* line, Capture& capture, char* reply, size_t reply_size)
{
    static const esp_desktop_buddy_command_extension_entry_t entries[] = {
        {"now_playing", handle_now_playing},
    };
    esp_desktop_buddy_config_t config{};
    config.handlers.command_extension.ctx = &capture;
    config.handlers.command_extension.bindings = entries;
    config.handlers.command_extension.binding_count = 1;

    esp_desktop_buddy_t* buddy = nullptr;
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_new(&config, &buddy));
    TEST_ASSERT_NOT_NULL(buddy);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_transport_port_attach(buddy));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_desktop_buddy_receive_bytes(
            buddy, reinterpret_cast<const uint8_t*>(line), strlen(line)));

    size_t reply_len = 0;
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_desktop_buddy_transport_port_next_frame(
            buddy, reinterpret_cast<uint8_t*>(reply), reply_size - 1,
            &reply_len, pdMS_TO_TICKS(1000)));
    reply[reply_len] = '\0';

    esp_desktop_buddy_transport_port_detach(buddy);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_delete(buddy));
}

}  // namespace

TEST_CASE("a now-playing command carries the song, the playhead and the lyric window", "[protocol]")
{
    Capture capture{};
    char reply[160]{};
    feed_line(kNowPlaying, capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(1, capture.accepted);
    TEST_ASSERT_EQUAL_STRING("证据", capture.update.title);
    TEST_ASSERT_EQUAL_STRING("杨乃文", capture.update.artist);
    TEST_ASSERT_EQUAL_UINT32(54, capture.update.position_seconds);
    TEST_ASSERT_EQUAL_UINT32(275, capture.update.duration_seconds);
    TEST_ASSERT_TRUE(capture.update.playing);
    TEST_ASSERT_EQUAL_UINT8(3, capture.update.line_count);
    TEST_ASSERT_EQUAL_UINT8(0, capture.update.current_line);
    TEST_ASSERT_EQUAL_STRING("被当作乱放电的诱惑", capture.update.lines[0]);
    TEST_ASSERT_EQUAL_STRING("难道都是我的错", capture.update.lines[2]);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ack\":\"now_playing\""));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"n\":42"));
}

TEST_CASE("the millisecond timeline travels with the lines", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(kNowPlaying, capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(1, capture.accepted);
    TEST_ASSERT_EQUAL_UINT32(54320, capture.update.position_ms);
    TEST_ASSERT_TRUE(capture.update.has_position);
    TEST_ASSERT_EQUAL_UINT8(3, capture.update.line_time_count);
    TEST_ASSERT_EQUAL_UINT32(52100, capture.update.line_times_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(55000, capture.update.line_times_ms[1]);
    TEST_ASSERT_EQUAL_UINT32(58000, capture.update.line_times_ms[2]);
}

TEST_CASE("a command without milliseconds leaves the panel on the seconds", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(
        "{\"cmd\":\"now_playing\",\"v\":\"1\",\"seq\":\"47\",\"position\":\"54\","
        "\"duration\":\"275\",\"state\":\"playing\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(1, capture.accepted);
    TEST_ASSERT_EQUAL_UINT32(0, capture.update.position_ms);
    TEST_ASSERT_FALSE(capture.update.has_position);
    TEST_ASSERT_EQUAL_UINT8(0, capture.update.line_time_count);
}

TEST_CASE("timestamps that do not address the lines are rejected", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(
        "{\"cmd\":\"now_playing\",\"v\":\"1\",\"seq\":\"48\",\"position\":\"54\","
        "\"duration\":\"275\",\"state\":\"playing\",\"lines\":\"一\\n二\","
        "\"line_times\":\"1000\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(0, capture.accepted);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ok\":false"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"invalid_request\""));
}

TEST_CASE("a timestamp that is not a canonical number is rejected", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(
        "{\"cmd\":\"now_playing\",\"v\":\"1\",\"seq\":\"49\",\"position\":\"54\","
        "\"duration\":\"275\",\"state\":\"playing\",\"lines\":\"一\","
        "\"line_times\":\"01000\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(0, capture.accepted);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ok\":false"));
}

TEST_CASE("an unknown player state is rejected", "[protocol]")
{
    Capture capture{};
    char reply[160]{};
    feed_line(kNowPlayingInvalid, capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(0, capture.accepted);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ok\":false"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"n\":43"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"invalid_request\""));
}

TEST_CASE("a missing playhead is rejected", "[protocol]")
{
    Capture capture{};
    char reply[160]{};
    feed_line(
        "{\"cmd\":\"now_playing\",\"v\":\"1\",\"seq\":\"44\",\"state\":\"playing\","
        "\"duration\":\"275\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(0, capture.accepted);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"n\":44"));
}

TEST_CASE("long text is truncated on a character boundary", "[protocol]")
{
    Capture capture{};
    char reply[160]{};
    // Seventeen three-byte characters: the copy must stop at fifteen, never mid-character.
    feed_line(
        "{\"cmd\":\"now_playing\",\"v\":\"1\",\"seq\":\"45\",\"position\":\"1\","
        "\"duration\":\"2\",\"state\":\"paused\",\"lines\":\"一二三四五六七八九十一二三四五六七\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(1, capture.accepted);
    const size_t length = strlen(capture.update.lines[0]);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(kMaxLyricLineBytes, length);
    TEST_ASSERT_EQUAL_UINT32(0, length % 3);
    TEST_ASSERT_FALSE(capture.update.playing);
}

TEST_CASE("more lines than the panel keeps are dropped", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(
        "{\"cmd\":\"now_playing\",\"v\":\"1\",\"seq\":\"46\",\"position\":\"1\","
        "\"duration\":\"2\",\"state\":\"playing\",\"current\":\"7\","
        "\"lines\":\"a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(1, capture.accepted);
    TEST_ASSERT_EQUAL_UINT8(kMaxLyricLines, capture.update.line_count);
    TEST_ASSERT_EQUAL_UINT8(7, capture.update.current_line);
}
