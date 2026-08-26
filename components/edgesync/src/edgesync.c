/**
 * @file edgesync.c
 * @brief Top-level lifecycle: wires storage, transport, queue and worker together.
 */
#include <string.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "edgesync/edgesync.h"
#include "edgesync_storage.h"
#include "edgesync_transport.h"
#include "edgesync_queue.h"
#include "edgesync_worker.h"
#include "edgesync_id.h"

static const char *TAG = "EdgeSync";

#ifndef CONFIG_EDGESYNC_MAX_QUEUE_SIZE
#define CONFIG_EDGESYNC_MAX_QUEUE_SIZE 256
#endif
#ifndef CONFIG_EDGESYNC_MAX_MESSAGE_SIZE
#define CONFIG_EDGESYNC_MAX_MESSAGE_SIZE 2048
#endif
#ifndef CONFIG_EDGESYNC_DEFAULT_RETRY_INITIAL_MS
#define CONFIG_EDGESYNC_DEFAULT_RETRY_INITIAL_MS 1000
#endif
#ifndef CONFIG_EDGESYNC_DEFAULT_RETRY_MAX_MS
#define CONFIG_EDGESYNC_DEFAULT_RETRY_MAX_MS 300000
#endif
#ifndef CONFIG_EDGESYNC_TASK_STACK_SIZE
#define CONFIG_EDGESYNC_TASK_STACK_SIZE 4096
#endif
#ifndef CONFIG_EDGESYNC_TASK_PRIORITY
#define CONFIG_EDGESYNC_TASK_PRIORITY 5
#endif

struct edgesync_ctx {
    edgesync_config_t config; /**< Copy; pointer fields borrowed per edgesync_init() contract. */

    const edgesync_storage_ops_t *storage_ops;
    void *storage_ctx;

    const edgesync_transport_ops_t *transport_ops;
    void *transport_ctx;

    edgesync_queue_t *queue;
    edgesync_worker_t *worker;

    edgesync_retry_config_t retry_cfg;
    uint32_t max_message_size;
    bool started;
};

esp_err_t edgesync_init(const edgesync_config_t *config, edgesync_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct edgesync_ctx *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->config = *config;

    uint32_t max_queue_size = config->max_queue_size ? config->max_queue_size : CONFIG_EDGESYNC_MAX_QUEUE_SIZE;
    ctx->max_message_size = config->max_message_size ? config->max_message_size : CONFIG_EDGESYNC_MAX_MESSAGE_SIZE;

    ctx->retry_cfg.initial_delay_ms = config->retry_initial_ms;
    ctx->retry_cfg.max_delay_ms = config->retry_max_ms;
    ctx->retry_cfg.multiplier = config->retry_multiplier;
    ctx->retry_cfg.jitter = config->retry_jitter;
    ctx->retry_cfg.max_attempts = config->retry_max_attempts;
    edgesync_retry_apply_defaults(&ctx->retry_cfg, CONFIG_EDGESYNC_DEFAULT_RETRY_INITIAL_MS,
                                  CONFIG_EDGESYNC_DEFAULT_RETRY_MAX_MS, 2);

    esp_err_t err = edgesync_storage_create(&ctx->config, max_queue_size, ctx->max_message_size,
                                             &ctx->storage_ops, &ctx->storage_ctx);
    if (err != ESP_OK) {
        free(ctx);
        return err;
    }

    err = edgesync_transport_create(&ctx->config, &ctx->transport_ops, &ctx->transport_ctx);
    if (err != ESP_OK) {
        edgesync_storage_destroy(&ctx->config, ctx->storage_ops, ctx->storage_ctx);
        free(ctx);
        return err;
    }

    if (ctx->transport_ops->init != NULL) {
        err = ctx->transport_ops->init(ctx->transport_ctx);
        if (err != ESP_OK) {
            edgesync_transport_destroy(&ctx->config, ctx->transport_ops, ctx->transport_ctx);
            edgesync_storage_destroy(&ctx->config, ctx->storage_ops, ctx->storage_ctx);
            free(ctx);
            return err;
        }
    }

    edgesync_queue_config_t queue_cfg = {
        .storage_ops = ctx->storage_ops,
        .storage_ctx = ctx->storage_ctx,
        .overflow_policy = config->overflow_policy,
        .event_cb = config->event_cb,
        .event_cb_ctx = config->event_cb_ctx,
    };
    err = edgesync_queue_create(&queue_cfg, &ctx->queue);
    if (err != ESP_OK) {
        edgesync_transport_destroy(&ctx->config, ctx->transport_ops, ctx->transport_ctx);
        edgesync_storage_destroy(&ctx->config, ctx->storage_ops, ctx->storage_ctx);
        free(ctx);
        return err;
    }

    err = edgesync_queue_recover(ctx->queue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage recovery failed: %s", esp_err_to_name(err));
        edgesync_queue_destroy(ctx->queue);
        edgesync_transport_destroy(&ctx->config, ctx->transport_ops, ctx->transport_ctx);
        edgesync_storage_destroy(&ctx->config, ctx->storage_ops, ctx->storage_ctx);
        free(ctx);
        return err;
    }

    *out_handle = ctx;
    return ESP_OK;
}

esp_err_t edgesync_start(edgesync_handle_t handle)
{
    if (handle == NULL) {
        return EDGESYNC_ERR_NOT_INITIALIZED;
    }
    if (handle->started) {
        return EDGESYNC_ERR_ALREADY_STARTED;
    }

    edgesync_worker_config_t worker_cfg = {
        .queue = handle->queue,
        .transport_ops = handle->transport_ops,
        .transport_ctx = handle->transport_ctx,
        .retry_cfg = handle->retry_cfg,
        .event_cb = handle->config.event_cb,
        .event_cb_ctx = handle->config.event_cb_ctx,
        .task_stack_size = handle->config.task_stack_size ? handle->config.task_stack_size : CONFIG_EDGESYNC_TASK_STACK_SIZE,
        .task_priority = handle->config.task_priority ? handle->config.task_priority : CONFIG_EDGESYNC_TASK_PRIORITY,
        .task_core_id = handle->config.task_core_id,
    };

    esp_err_t err = edgesync_worker_start(&worker_cfg, &handle->worker);
    if (err != ESP_OK) {
        return err;
    }

    handle->started = true;
    return ESP_OK;
}

esp_err_t edgesync_publish(edgesync_handle_t handle,
                            const char *destination,
                            const void *data,
                            size_t data_len,
                            const edgesync_publish_options_t *options)
{
    if (handle == NULL || !handle->started) {
        return EDGESYNC_ERR_NOT_INITIALIZED;
    }
    if (destination == NULL || (data_len > 0 && data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t dest_len = strlen(destination);
    if (dest_len == 0 || dest_len > EDGESYNC_MAX_DESTINATION_LEN || data_len > handle->max_message_size) {
        return EDGESYNC_ERR_MESSAGE_TOO_LARGE;
    }

    edgesync_message_t message = {0};
    message.id = (options != NULL && options->message_id != EDGESYNC_MESSAGE_ID_INVALID)
                     ? options->message_id : edgesync_id_generate();
    memcpy(message.destination, destination, dest_len);
    message.destination[dest_len] = '\0';
    message.priority = options != NULL ? options->priority : EDGESYNC_PRIORITY_NORMAL;
    message.state = EDGESYNC_MSG_PENDING;
    message.created_at_us = esp_timer_get_time();
    message.last_error = ESP_OK;
    message.payload_len = (uint32_t)data_len;
    message.payload = data; /* Borrowed for the duration of edgesync_queue_publish() only. */

    return edgesync_queue_publish(handle->queue, &message);
}

esp_err_t edgesync_stop(edgesync_handle_t handle)
{
    if (handle == NULL) {
        return EDGESYNC_ERR_NOT_INITIALIZED;
    }
    if (!handle->started) {
        return EDGESYNC_ERR_INVALID_STATE;
    }

    edgesync_worker_stop(handle->worker);
    handle->worker = NULL;
    handle->started = false;
    return ESP_OK;
}

esp_err_t edgesync_deinit(edgesync_handle_t handle)
{
    if (handle == NULL) {
        return EDGESYNC_ERR_NOT_INITIALIZED;
    }
    if (handle->started) {
        return EDGESYNC_ERR_INVALID_STATE;
    }

    edgesync_queue_destroy(handle->queue);
    edgesync_transport_destroy(&handle->config, handle->transport_ops, handle->transport_ctx);
    edgesync_storage_destroy(&handle->config, handle->storage_ops, handle->storage_ctx);
    free(handle);
    return ESP_OK;
}

esp_err_t edgesync_get_stats(edgesync_handle_t handle, edgesync_stats_t *stats)
{
    if (handle == NULL) {
        return EDGESYNC_ERR_NOT_INITIALIZED;
    }
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return edgesync_queue_get_stats(handle->queue, stats);
}
