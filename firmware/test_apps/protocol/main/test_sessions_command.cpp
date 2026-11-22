#include <cstring>

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/transport_port.h"
#include "fixture_data.hpp"
#include "unity.h"
#include "usage_protocol/sessions_command.hpp"

using namespace usage_panel;

namespace {

struct Capture {
    unsigned accepted = 0;
    SessionsUpdate update{};
};

esp_desktop_buddy_command_extension_result_t handle_sessions(
    void* context,
    esp_desktop_buddy_t*,
    const esp_desktop_buddy_command_view_t* request)
{
    auto* capture = static_cast<Capture*>(context);
    const auto parsed = parse_sessions_command(request);
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
        {"sessions", handle_sessions},
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

TEST_CASE("a sessions command carries the live roster, its states and the recent titles", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(kSessions, capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(1, capture.accepted);
    TEST_ASSERT_EQUAL_UINT32(46, capture.update.sequence);
    TEST_ASSERT_EQUAL_UINT8(2, capture.update.live_count);
    TEST_ASSERT_EQUAL_STRING("Claude Code Desktop 会话状态页面", capture.update.live[0].name);
    TEST_ASSERT_EQUAL(SessionState::Busy, capture.update.live[0].state);
    TEST_ASSERT_EQUAL_STRING("修复列表滚动的问题", capture.update.live[1].name);
    TEST_ASSERT_EQUAL(SessionState::Idle, capture.update.live[1].state);
    TEST_ASSERT_EQUAL_UINT8(2, capture.update.recent_count);
    TEST_ASSERT_EQUAL_STRING("给面板加一个秒表页", capture.update.recent[0].title);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ack\":\"sessions\""));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"n\":46"));
}

TEST_CASE("states that do not address the roster are rejected", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(kSessionsInvalid, capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(0, capture.accepted);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ok\":false"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"invalid_request\""));
}

TEST_CASE("a roster without its states is rejected", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(
        "{\"cmd\":\"sessions\",\"v\":\"1\",\"seq\":\"47\",\"live\":\"只有一个名字\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(0, capture.accepted);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"invalid_request\""));
}

TEST_CASE("an unknown session state is rejected", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(
        "{\"cmd\":\"sessions\",\"v\":\"1\",\"seq\":\"48\",\"live\":\"a\",\"states\":\"stopped\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(0, capture.accepted);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"invalid_request\""));
}

TEST_CASE("a command without any roster is accepted and leaves the page empty", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(
        "{\"cmd\":\"sessions\",\"v\":\"1\",\"seq\":\"49\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(1, capture.accepted);
    TEST_ASSERT_EQUAL_UINT8(0, capture.update.live_count);
    TEST_ASSERT_EQUAL_UINT8(0, capture.update.recent_count);
}

TEST_CASE("a version other than one is rejected", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    feed_line(
        "{\"cmd\":\"sessions\",\"v\":\"2\",\"seq\":\"50\",\"live\":\"a\",\"states\":\"busy\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(0, capture.accepted);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"invalid_request\""));
}

TEST_CASE("long session names are truncated on a character boundary", "[protocol]")
{
    Capture capture{};
    char reply[200]{};
    // Forty four three-byte characters: the copy must stop at thirty-two.
    feed_line(
        "{\"cmd\":\"sessions\",\"v\":\"1\",\"seq\":\"51\",\"live\":\"一二三四五六七八九十"
        "一二三四五六七八九十 一二三四五六七八九十 一二三四五六七八九十 一二三四\","
        "\"states\":\"busy\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(1, capture.accepted);
    const size_t length = strlen(capture.update.live[0].name);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(kMaxSessionTextBytes, length);
    TEST_ASSERT_EQUAL_UINT32(0, length % 3);
}

TEST_CASE("more live sessions than the panel keeps are dropped", "[protocol]")
{
    Capture capture{};
    char reply[256]{};
    feed_line(
        "{\"cmd\":\"sessions\",\"v\":\"1\",\"seq\":\"52\",\"live\":\"a\\nb\\nc\\nd\\ne\\nf\","
        "\"states\":\"busy\\nbusy\\nbusy\\nbusy\\nbusy\\nbusy\"}\n",
        capture, reply, sizeof(reply));

    TEST_ASSERT_EQUAL_UINT32(1, capture.accepted);
    TEST_ASSERT_EQUAL_UINT8(kMaxLiveSessions, capture.update.live_count);
}
