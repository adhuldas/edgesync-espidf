/**
 * @file edgesync_http.c
 * @brief Built-in HTTP(S) transport implementation.
 *
 * One esp_http_client instance is created per configured transport and
 * reused across deliveries (deliver() is only ever called from the
 * single EdgeSync worker task, so no locking is needed here). TLS
 * verification is enabled by default via the ESP-IDF certificate bundle;
 * see README.md "Security" for what is and is not verified.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "edgesync/edgesync.h"
#include "edgesync_http.h"
#include "edgesync_id.h"

static const char *TAG = "EdgeSync";

#ifndef CONFIG_EDGESYNC_HTTP_DEFAULT_TIMEOUT_MS
#define CONFIG_EDGESYNC_HTTP_DEFAULT_TIMEOUT_MS 30000
#endif

#define EDGESYNC_HTTP_MAX_URL_LEN 256

typedef struct {
    edgesync_http_transport_config_t config;
    char base_url[EDGESYNC_HTTP_MAX_URL_LEN];
} edgesync_http_ctx_t;

static edgesync_delivery_result_t default_classify(int status)
{
    if (status <= 0) {
        return EDGESYNC_DELIVERY_RETRYABLE_FAILURE; /* no response: connect/DNS/timeout failure */
    }
    if (status >= 200 && status < 300) {
        return EDGESYNC_DELIVERY_SUCCESS;
    }
    switch (status) {
    case 400:
    case 401:
    case 403:
    case 404:
    case 422:
        return EDGESYNC_DELIVERY_PERMANENT_FAILURE;
    case 408:
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
        return EDGESYNC_DELIVERY_RETRYABLE_FAILURE;
    default:
        /* Unlisted codes: treat 5xx as transient, everything else (3xx not
         * followed, unusual 4xx) as permanent - a safer default than
         * retrying forever against a destination that will never accept it. */
        return (status >= 500) ? EDGESYNC_DELIVERY_RETRYABLE_FAILURE : EDGESYNC_DELIVERY_PERMANENT_FAILURE;
    }
}

static esp_err_t op_init(void *vctx)
{
    (void)vctx;
    return ESP_OK;
}

static edgesync_delivery_result_t op_deliver(void *vctx, const edgesync_message_t *message, int *out_status_code)
{
    edgesync_http_ctx_t *ctx = vctx;
    if (out_status_code) {
        *out_status_code = 0;
    }

    char url[EDGESYNC_HTTP_MAX_URL_LEN];
    if (ctx->config.no_append_destination) {
        strlcpy(url, ctx->base_url, sizeof(url));
    } else {
        int n = snprintf(url, sizeof(url), "%s/%s", ctx->base_url, message->destination);
        if (n < 0 || (size_t)n >= sizeof(url)) {
            ESP_LOGE(TAG, "constructed URL too long, dropping message %016llx", (unsigned long long)message->id);
            return EDGESYNC_DELIVERY_PERMANENT_FAILURE;
        }
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)(ctx->config.timeout_ms ? ctx->config.timeout_ms : CONFIG_EDGESYNC_HTTP_DEFAULT_TIMEOUT_MS),
        .skip_cert_common_name_check = ctx->config.skip_cert_common_name_check,
    };
    if (ctx->config.cert_pem) {
        http_cfg.cert_pem = ctx->config.cert_pem;
    } else if (ctx->config.use_cert_bundle) {
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return EDGESYNC_DELIVERY_RETRYABLE_FAILURE;
    }

    const char *content_type = ctx->config.content_type ? ctx->config.content_type : "application/octet-stream";
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "X-EdgeSync-Destination", message->destination);

    const char *idem_header = ctx->config.idempotency_header ? ctx->config.idempotency_header : "Idempotency-Key";
    if (idem_header[0] != '\0') {
        char hex[17];
        edgesync_id_to_hex(message->id, hex);
        esp_http_client_set_header(client, idem_header, hex);
    }

    if (ctx->config.auth_header_name && ctx->config.auth_header_value) {
        esp_http_client_set_header(client, ctx->config.auth_header_name, ctx->config.auth_header_value);
    }
    for (size_t i = 0; i < ctx->config.extra_header_count; i++) {
        esp_http_client_set_header(client, ctx->config.extra_header_names[i], ctx->config.extra_header_values[i]);
    }

    esp_http_client_set_post_field(client, (const char *)message->payload, (int)message->payload_len);

    esp_err_t err = esp_http_client_perform(client);
    edgesync_delivery_result_t result;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed for message %016llx: %s",
                 (unsigned long long)message->id, esp_err_to_name(err));
        result = EDGESYNC_DELIVERY_RETRYABLE_FAILURE; /* connection/DNS/TLS/timeout failure */
    } else {
        int status = esp_http_client_get_status_code(client);
        if (out_status_code) {
            *out_status_code = status;
        }
        result = ctx->config.classify_status ? ctx->config.classify_status(status) : (edgesync_delivery_result_t)-1;
        if ((int)result < 0) {
            result = default_classify(status);
        }
        ESP_LOGD(TAG, "message %016llx -> HTTP %d -> %s", (unsigned long long)message->id, status,
                 result == EDGESYNC_DELIVERY_SUCCESS ? "success" :
                 result == EDGESYNC_DELIVERY_RETRYABLE_FAILURE ? "retryable" : "permanent");
    }

    esp_http_client_cleanup(client);
    return result;
}

static esp_err_t op_close(void *vctx)
{
    (void)vctx;
    return ESP_OK;
}

static const edgesync_transport_ops_t s_http_ops = {
    .init = op_init,
    .deliver = op_deliver,
    .close = op_close,
};

esp_err_t edgesync_http_transport_create(const edgesync_http_transport_config_t *config,
                                          const edgesync_transport_ops_t **out_ops, void **out_ctx)
{
    if (config == NULL || config->url == NULL || out_ops == NULL || out_ctx == NULL) {
        return EDGESYNC_ERR_TRANSPORT;
    }

    edgesync_http_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ctx->config = *config;
    strlcpy(ctx->base_url, config->url, sizeof(ctx->base_url));
    if (ctx->config.use_cert_bundle == false && ctx->config.cert_pem == NULL &&
        strncmp(ctx->base_url, "https://", 8) == 0) {
        /* Default to the bundled CA store for HTTPS URLs unless the caller
         * explicitly opted out - never silently disable TLS verification. */
        ctx->config.use_cert_bundle = true;
    }

    *out_ops = &s_http_ops;
    *out_ctx = ctx;
    return ESP_OK;
}

void edgesync_http_transport_destroy(void *vctx)
{
    free(vctx);
}
