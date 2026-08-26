/**
 * EdgeSync over the built-in HTTP(S) transport.
 *
 * Connects to Wi-Fi (see `idf.py menuconfig` -> "EdgeSync HTTP Example" for
 * SSID/password/endpoint), then publishes a small JSON payload every few
 * seconds. Because edgesync_publish() never blocks on the network, messages
 * queue up durably even before Wi-Fi finishes connecting, or if it drops
 * later - EdgeSync drains them as connectivity allows.
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "edgesync/edgesync.h"

static const char *TAG = "example";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_station(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_EXAMPLE_WIFI_SSID,
            .password = CONFIG_EXAMPLE_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to \"%s\"...", CONFIG_EXAMPLE_WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "connected");
}

static void on_event(edgesync_event_id_t event, const edgesync_message_t *msg, void *ctx)
{
    (void)ctx;
    static const char *names[] = {
        "QUEUED", "DELIVERED", "RETRY", "FAILED", "QUEUE_FULL", "DEAD_LETTER",
    };
    ESP_LOGI(TAG, "event %s for message %016llx (attempt %u)",
             names[event], (unsigned long long)msg->id, (unsigned)msg->attempt_count);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_station();

    edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
    config.http.url = CONFIG_EXAMPLE_HTTP_ENDPOINT;
    config.http.no_append_destination = true; /* see Kconfig help for CONFIG_EXAMPLE_HTTP_ENDPOINT */
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

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
