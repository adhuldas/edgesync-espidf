/**
 * @file edgesync_mqtt.c
 * @brief Built-in MQTT transport implementation.
 *
 * One esp-mqtt client is created per configured transport and kept
 * connected (with esp-mqtt's own auto-reconnect) for the transport's
 * lifetime; deliver() only ever runs from the single EdgeSync worker task,
 * so a single-slot ack queue is enough to hand the MQTT event-handler task's
 * MQTT_EVENT_PUBLISHED notification back to whichever deliver() call is
 * waiting on it.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "edgesync/edgesync.h"
#include "edgesync/edgesync_mqtt.h"

static const char *TAG = "EdgeSync";

#ifndef CONFIG_EDGESYNC_MQTT_DEFAULT_TIMEOUT_MS
#define CONFIG_EDGESYNC_MQTT_DEFAULT_TIMEOUT_MS 10000
#endif

#ifndef CONFIG_EDGESYNC_MQTT_DEFAULT_QOS
#define CONFIG_EDGESYNC_MQTT_DEFAULT_QOS 1
#endif

#define EDGESYNC_MQTT_MAX_TOPIC_LEN 256

typedef struct {
    edgesync_mqtt_transport_config_t config;
    char base_topic[EDGESYNC_MQTT_MAX_TOPIC_LEN];
    esp_mqtt_client_handle_t client;
    QueueHandle_t ack_queue; /**< Holds the msg_id from the most recent MQTT_EVENT_PUBLISHED. */
} edgesync_mqtt_ctx_t;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    edgesync_mqtt_ctx_t *ctx = handler_args;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to broker");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected; esp-mqtt will auto-reconnect");
        break;
    case MQTT_EVENT_PUBLISHED:
        xQueueOverwrite(ctx->ack_queue, &event->msg_id);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error (type=%d)", event->error_handle ? (int)event->error_handle->error_type : -1);
        break;
    default:
        break;
    }
}

static esp_err_t op_init(void *vctx)
{
    edgesync_mqtt_ctx_t *ctx = vctx;
    esp_err_t err = esp_mqtt_client_register_event(ctx->client, ESP_EVENT_ANY_ID, mqtt_event_handler, ctx);
    if (err != ESP_OK) {
        return err;
    }
    return esp_mqtt_client_start(ctx->client);
}

static edgesync_delivery_result_t op_deliver(void *vctx, const edgesync_message_t *message, int *out_status_code)
{
    edgesync_mqtt_ctx_t *ctx = vctx;
    if (out_status_code) {
        *out_status_code = 0;
    }

    char topic[EDGESYNC_MQTT_MAX_TOPIC_LEN];
    if (ctx->config.no_append_destination) {
        strlcpy(topic, ctx->base_topic, sizeof(topic));
    } else {
        int n = snprintf(topic, sizeof(topic), "%s/%s", ctx->base_topic, message->destination);
        if (n < 0 || (size_t)n >= sizeof(topic)) {
            ESP_LOGE(TAG, "constructed topic too long, dropping message %016llx", (unsigned long long)message->id);
            return EDGESYNC_DELIVERY_PERMANENT_FAILURE;
        }
    }

    int qos = ctx->config.qos;
    if (qos < 0 || qos > 2) {
        qos = CONFIG_EDGESYNC_MQTT_DEFAULT_QOS;
    }

    /* Single message in flight at a time (deliver() is serial), so drop any
     * stale ack left over from a previous call before publishing this one. */
    xQueueReset(ctx->ack_queue);

    int msg_id = esp_mqtt_client_publish(ctx->client, topic, (const char *)message->payload,
                                          (int)message->payload_len, qos, ctx->config.retain);
    if (out_status_code) {
        *out_status_code = msg_id;
    }
    if (msg_id < 0) {
        ESP_LOGW(TAG, "MQTT publish failed for message %016llx (not connected or write error)",
                 (unsigned long long)message->id);
        return EDGESYNC_DELIVERY_RETRYABLE_FAILURE;
    }

    uint32_t timeout_ms = ctx->config.timeout_ms ? ctx->config.timeout_ms : CONFIG_EDGESYNC_MQTT_DEFAULT_TIMEOUT_MS;
    int acked_id = -1;
    if (xQueueReceive(ctx->ack_queue, &acked_id, pdMS_TO_TICKS(timeout_ms)) == pdTRUE && acked_id == msg_id) {
        return EDGESYNC_DELIVERY_SUCCESS;
    }

    ESP_LOGW(TAG, "MQTT publish for message %016llx not acknowledged within %ums",
             (unsigned long long)message->id, (unsigned)timeout_ms);
    return EDGESYNC_DELIVERY_RETRYABLE_FAILURE;
}

static esp_err_t op_close(void *vctx)
{
    edgesync_mqtt_ctx_t *ctx = vctx;
    return esp_mqtt_client_stop(ctx->client);
}

static const edgesync_transport_ops_t s_mqtt_ops = {
    .init = op_init,
    .deliver = op_deliver,
    .close = op_close,
};

esp_err_t edgesync_mqtt_transport_create(const edgesync_mqtt_transport_config_t *config,
                                          const edgesync_transport_ops_t **out_ops, void **out_ctx)
{
    if (config == NULL || config->broker_uri == NULL || config->topic == NULL ||
        out_ops == NULL || out_ctx == NULL) {
        return EDGESYNC_ERR_TRANSPORT;
    }

    edgesync_mqtt_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ctx->config = *config;
    strlcpy(ctx->base_topic, config->topic, sizeof(ctx->base_topic));

    bool use_cert_bundle = ctx->config.use_cert_bundle;
    if (!use_cert_bundle && ctx->config.cert_pem == NULL && strncmp(config->broker_uri, "mqtts://", 8) == 0) {
        /* Default to the bundled CA store for mqtts:// URIs unless the
         * caller explicitly opted out - never silently disable TLS
         * verification. */
        use_cert_bundle = true;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config->broker_uri,
        .broker.verification.certificate = ctx->config.cert_pem,
        .broker.verification.skip_cert_common_name_check = ctx->config.skip_cert_common_name_check,
        .credentials.username = ctx->config.username,
        .credentials.client_id = ctx->config.client_id,
        .credentials.authentication.password = ctx->config.password,
        .credentials.authentication.certificate = ctx->config.client_cert_pem,
        .credentials.authentication.key = ctx->config.client_key_pem,
    };
    if (use_cert_bundle && ctx->config.cert_pem == NULL) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    ctx->client = esp_mqtt_client_init(&mqtt_cfg);
    if (ctx->client == NULL) {
        free(ctx);
        return EDGESYNC_ERR_TRANSPORT;
    }

    ctx->ack_queue = xQueueCreate(1, sizeof(int));
    if (ctx->ack_queue == NULL) {
        esp_mqtt_client_destroy(ctx->client);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    *out_ops = &s_mqtt_ops;
    *out_ctx = ctx;
    return ESP_OK;
}

void edgesync_mqtt_transport_destroy(void *vctx)
{
    edgesync_mqtt_ctx_t *ctx = vctx;
    if (ctx == NULL) {
        return;
    }
    if (ctx->client) {
        esp_mqtt_client_destroy(ctx->client);
    }
    if (ctx->ack_queue) {
        vQueueDelete(ctx->ack_queue);
    }
    free(ctx);
}
