/**
 * Minimal EdgeSync example: no network stack required.
 *
 * Demonstrates the durable-queue behavior on its own, using a trivial
 * "transport" that just logs each message instead of sending it anywhere.
 * Swap in the built-in HTTP(S) transport (see the "http" example) or your
 * own edgesync_transport_ops_t once you have real connectivity.
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "edgesync/edgesync.h"

static const char *TAG = "example";

static edgesync_delivery_result_t logging_deliver(void *ctx, const edgesync_message_t *msg, int *status)
{
    (void)ctx;
    if (status) {
        *status = 200;
    }
    ESP_LOGI(TAG, "delivering [%s] (%u bytes, attempt %u): %.*s",
             msg->destination, (unsigned)msg->payload_len, (unsigned)msg->attempt_count,
             (int)msg->payload_len, (const char *)msg->payload);
    return EDGESYNC_DELIVERY_SUCCESS;
}

static const edgesync_transport_ops_t logging_transport = {
    .deliver = logging_deliver,
};

static void on_event(edgesync_event_id_t event, const edgesync_message_t *msg, void *ctx)
{
    (void)ctx;
    static const char *names[] = {
        "QUEUED", "DELIVERED", "RETRY", "FAILED", "QUEUE_FULL", "DEAD_LETTER",
    };
    ESP_LOGI(TAG, "event %s for message %016llx", names[event], (unsigned long long)msg->id);
}

void app_main(void)
{
    edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
    config.transport_ops = &logging_transport;
    config.event_cb = on_event;

    edgesync_handle_t handle;
    ESP_ERROR_CHECK(edgesync_init(&config, &handle));
    ESP_ERROR_CHECK(edgesync_start(handle));

    for (int i = 0; ; i++) {
        char payload[64];
        int len = snprintf(payload, sizeof(payload), "{\"seq\": %d}", i);

        esp_err_t err = edgesync_publish(handle, "telemetry", payload, (size_t)len, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "publish failed: %s", esp_err_to_name(err));
        }

        edgesync_stats_t stats;
        if (edgesync_get_stats(handle, &stats) == ESP_OK) {
            ESP_LOGI(TAG, "pending=%u in_flight=%u delivered_awaiting_gc=%u since_boot(ok=%llu, fail=%llu)",
                     stats.pending_messages, stats.in_flight_messages, stats.delivered_awaiting_gc,
                     (unsigned long long)stats.successful_deliveries,
                     (unsigned long long)stats.failed_deliveries);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
