#include <cstring>

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/transport_port.h"
#include "unity.h"
#include "usage_ble/ble_service.hpp"

using namespace usage_panel;

namespace {

struct RouteContext {
    AppEventQueue* queue;
    bool enabled;
    bool ota_busy = false;
};

esp_desktop_buddy_command_extension_result_t route(
    void* context,
    esp_desktop_buddy_t*,
    const esp_desktop_buddy_command_view_t* request)
{
    const auto* route_context = static_cast<RouteContext*>(context);
    return route_alert_set_command(
        request, route_context->enabled, route_context->ota_busy,
        route_context->queue);
}

void feed_line(const char* line, RouteContext& context, char* reply, size_t reply_size)
{
    static const esp_desktop_buddy_command_extension_entry_t entries[] = {
        {"alert_set", route},
    };
    esp_desktop_buddy_config_t config{};
    config.handlers.command_extension.ctx = &context;
    config.handlers.command_extension.bindings = entries;
    config.handlers.command_extension.binding_count = 1;

    esp_desktop_buddy_t* buddy = nullptr;
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_new(&config, &buddy));
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

constexpr char kBeep[] = "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"47\",\"mode\":\"2\"}\n";

}  // namespace

TEST_CASE("alert_set queues the requested mode once", "[ble_alert_set]")
{
    AppEventQueue queue;
    TEST_ASSERT_TRUE(queue.begin(1));
    RouteContext context{&queue, true};
    char reply[160]{};
    feed_line(kBeep, context, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"n\":47"));

    AppEvent event{};
    TEST_ASSERT_TRUE(queue.receive(event));
    TEST_ASSERT_EQUAL(int(AppEventType::AlertSet), int(event.type));
    TEST_ASSERT_EQUAL_UINT8(2, event.alert_mode);
    TEST_ASSERT_FALSE(queue.receive(event));

    // A full queue is reported instead of blocking the BLE callback.
    feed_line(kBeep, context, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "queue_full"));
}

TEST_CASE("alert_set accepts every contract mode and rejects the rest", "[ble_alert_set]")
{
    AppEventQueue queue;
    TEST_ASSERT_TRUE(queue.begin(8));
    RouteContext context{&queue, true};
    char reply[160]{};
    const char* modes[] = {
        "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"1\",\"mode\":\"0\"}\n",
        "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"2\",\"mode\":\"1\"}\n",
        "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"3\",\"mode\":\"2\"}\n",
    };
    for (const char* line : modes) {
        feed_line(line, context, reply, sizeof(reply));
        TEST_ASSERT_NOT_NULL(strstr(reply, "\"ok\":true"));
        AppEvent event{};
        TEST_ASSERT_TRUE(queue.receive(event));
        TEST_ASSERT_EQUAL(int(AppEventType::AlertSet), int(event.type));
    }

    const char* invalid[] = {
        "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"4\",\"mode\":\"3\"}\n",
        "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"5\",\"mode\":\"02\"}\n",
        "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"6\",\"mode\":\"-1\"}\n",
        "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"7\",\"mode\":\"2.0\"}\n",
        "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"8\"}\n",
        "{\"cmd\":\"alert_set\",\"v\":\"2\",\"seq\":\"9\",\"mode\":\"2\"}\n",
        "{\"cmd\":\"alert_set\",\"v\":\"1\",\"seq\":\"4294967296\",\"mode\":\"2\"}\n",
    };
    for (const char* line : invalid) {
        feed_line(line, context, reply, sizeof(reply));
        TEST_ASSERT_NOT_NULL(strstr(reply, "invalid_request"));
    }
    AppEvent event{};
    TEST_ASSERT_FALSE(queue.receive(event));
}

TEST_CASE("alert_set reports capability and OTA state before queueing", "[ble_alert_set]")
{
    AppEventQueue queue;
    TEST_ASSERT_TRUE(queue.begin());
    RouteContext context{&queue, false};
    char reply[160]{};
    feed_line(kBeep, context, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"ok\":false"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"unsupported\""));

    context.enabled = true;
    context.ota_busy = true;
    feed_line(kBeep, context, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"error\":\"ota_busy\""));

    AppEvent event{};
    TEST_ASSERT_FALSE(queue.receive(event));
}
