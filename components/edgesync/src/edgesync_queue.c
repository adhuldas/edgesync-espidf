#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "edgesync_queue.h"
#include "edgesync_events.h"

struct edgesync_queue {
    const edgesync_storage_ops_t *storage_ops;
    void *storage_ctx;
    edgesync_overflow_policy_t overflow_policy;
    edgesync_event_cb_t event_cb;
    void *event_cb_ctx;

    SemaphoreHandle_t lock;

    void (*wake_cb)(void *ctx);
    void *wake_ctx;

    uint64_t successful_deliveries;
    uint64_t failed_deliveries;
};

esp_err_t edgesync_queue_create(const edgesync_queue_config_t *config, edgesync_queue_t **out_queue)
{
    if (config == NULL || config->storage_ops == NULL || out_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    edgesync_queue_t *q = calloc(1, sizeof(*q));
    if (q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    q->lock = xSemaphoreCreateMutex();
    if (q->lock == NULL) {
        free(q);
        return ESP_ERR_NO_MEM;
    }

    q->storage_ops = config->storage_ops;
    q->storage_ctx = config->storage_ctx;
    q->overflow_policy = config->overflow_policy;
    q->event_cb = config->event_cb;
    q->event_cb_ctx = config->event_cb_ctx;

    *out_queue = q;
    return ESP_OK;
}

esp_err_t edgesync_queue_recover(edgesync_queue_t *queue)
{
    esp_err_t err = queue->storage_ops->init(queue->storage_ctx);
    if (err != ESP_OK) {
        return err;
    }
    return queue->storage_ops->recover(queue->storage_ctx);
}

void edgesync_queue_set_wake_cb(edgesync_queue_t *queue, void (*wake_cb)(void *ctx), void *wake_ctx)
{
    xSemaphoreTake(queue->lock, portMAX_DELAY);
    queue->wake_cb = wake_cb;
    queue->wake_ctx = wake_ctx;
    xSemaphoreGive(queue->lock);
}

esp_err_t edgesync_queue_publish(edgesync_queue_t *queue, const edgesync_message_t *message)
{
    xSemaphoreTake(queue->lock, portMAX_DELAY);

    esp_err_t err = queue->storage_ops->enqueue(queue->storage_ctx, message);

    bool dropped = false;
    edgesync_message_id_t dropped_id = EDGESYNC_MESSAGE_ID_INVALID;

    if (err == EDGESYNC_ERR_QUEUE_FULL && queue->overflow_policy != EDGESYNC_OVERFLOW_REJECT_NEW) {
        if (queue->storage_ops->find_drop_candidate(queue->storage_ctx, queue->overflow_policy, &dropped_id) == ESP_OK) {
            queue->storage_ops->remove(queue->storage_ctx, dropped_id);
            dropped = true;
            err = queue->storage_ops->enqueue(queue->storage_ctx, message);
        }
    }

    void (*wake_cb)(void *) = (err == ESP_OK) ? queue->wake_cb : NULL;
    void *wake_ctx = queue->wake_ctx;

    xSemaphoreGive(queue->lock);

    if (dropped) {
        edgesync_message_t dropped_msg = {0};
        dropped_msg.id = dropped_id;
        edgesync_events_fire(queue->event_cb, queue->event_cb_ctx, EDGESYNC_EVENT_QUEUE_FULL, &dropped_msg);
    }

    if (err == ESP_OK) {
        edgesync_events_fire(queue->event_cb, queue->event_cb_ctx, EDGESYNC_EVENT_QUEUED, message);
        if (wake_cb != NULL) {
            wake_cb(wake_ctx);
        }
    } else if (err == EDGESYNC_ERR_QUEUE_FULL) {
        edgesync_events_fire(queue->event_cb, queue->event_cb_ctx, EDGESYNC_EVENT_QUEUE_FULL, message);
    }

    return err;
}

esp_err_t edgesync_queue_claim_next(edgesync_queue_t *queue, int64_t now_us, edgesync_message_t *out_message)
{
    xSemaphoreTake(queue->lock, portMAX_DELAY);
    esp_err_t err = queue->storage_ops->claim_next(queue->storage_ctx, now_us, out_message);
    xSemaphoreGive(queue->lock);
    return err;
}

esp_err_t edgesync_queue_mark_delivered(edgesync_queue_t *queue, edgesync_message_id_t id)
{
    xSemaphoreTake(queue->lock, portMAX_DELAY);
    esp_err_t err = queue->storage_ops->mark_delivered(queue->storage_ctx, id);
    if (err == ESP_OK) {
        queue->successful_deliveries++;
    }
    xSemaphoreGive(queue->lock);
    return err;
}

esp_err_t edgesync_queue_schedule_retry(edgesync_queue_t *queue, edgesync_message_id_t id,
                                         int64_t next_retry_at_us, bool dead_letter, esp_err_t last_error)
{
    xSemaphoreTake(queue->lock, portMAX_DELAY);
    esp_err_t err = queue->storage_ops->schedule_retry(queue->storage_ctx, id, next_retry_at_us,
                                                         dead_letter, last_error);
    if (err == ESP_OK) {
        queue->failed_deliveries++;
    }
    xSemaphoreGive(queue->lock);
    return err;
}

esp_err_t edgesync_queue_get_stats(edgesync_queue_t *queue, edgesync_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    xSemaphoreTake(queue->lock, portMAX_DELAY);
    esp_err_t err = queue->storage_ops->get_stats(queue->storage_ctx, stats);
    stats->successful_deliveries = queue->successful_deliveries;
    stats->failed_deliveries = queue->failed_deliveries;
    xSemaphoreGive(queue->lock);
    return err;
}

void edgesync_queue_destroy(edgesync_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    if (queue->lock != NULL) {
        vSemaphoreDelete(queue->lock);
    }
    free(queue);
}
