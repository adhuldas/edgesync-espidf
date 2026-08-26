#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "edgesync_worker.h"
#include "edgesync_events.h"

static const char *TAG = "EdgeSync";

#ifndef CONFIG_EDGESYNC_WORKER_IDLE_POLL_MS
#define CONFIG_EDGESYNC_WORKER_IDLE_POLL_MS 5000
#endif

struct edgesync_worker {
    edgesync_queue_t *queue;
    const edgesync_transport_ops_t *transport_ops;
    void *transport_ctx;
    edgesync_retry_config_t retry_cfg;
    edgesync_event_cb_t event_cb;
    void *event_cb_ctx;

    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    volatile bool stop_requested;
};

static void wake_cb(void *ctx)
{
    edgesync_worker_t *w = ctx;
    xTaskNotifyGive(w->task);
}

/** Schedules a retry or dead-letters the message, firing the matching events. */
static void handle_retryable_failure(edgesync_worker_t *w, const edgesync_message_t *msg, int64_t now_us)
{
    if (!edgesync_retry_should_retry(&w->retry_cfg, msg->attempt_count)) {
        edgesync_queue_schedule_retry(w->queue, msg->id, 0, true, EDGESYNC_ERR_TRANSPORT);
        edgesync_events_fire(w->event_cb, w->event_cb_ctx, EDGESYNC_EVENT_FAILED, msg);
        edgesync_events_fire(w->event_cb, w->event_cb_ctx, EDGESYNC_EVENT_DEAD_LETTER, msg);
        return;
    }

    uint32_t delay_ms = edgesync_retry_compute_delay_ms(&w->retry_cfg, msg->attempt_count, esp_random());
    int64_t next_retry_at_us = now_us + (int64_t)delay_ms * 1000;
    edgesync_queue_schedule_retry(w->queue, msg->id, next_retry_at_us, false, EDGESYNC_ERR_TRANSPORT);
    edgesync_events_fire(w->event_cb, w->event_cb_ctx, EDGESYNC_EVENT_FAILED, msg);
    edgesync_events_fire(w->event_cb, w->event_cb_ctx, EDGESYNC_EVENT_RETRY, msg);
}

static void handle_delivery_result(edgesync_worker_t *w, const edgesync_message_t *msg,
                                    edgesync_delivery_result_t result)
{
    switch (result) {
    case EDGESYNC_DELIVERY_SUCCESS:
        edgesync_queue_mark_delivered(w->queue, msg->id);
        edgesync_events_fire(w->event_cb, w->event_cb_ctx, EDGESYNC_EVENT_DELIVERED, msg);
        break;

    case EDGESYNC_DELIVERY_PERMANENT_FAILURE:
        edgesync_queue_schedule_retry(w->queue, msg->id, 0, true, EDGESYNC_ERR_TRANSPORT);
        edgesync_events_fire(w->event_cb, w->event_cb_ctx, EDGESYNC_EVENT_FAILED, msg);
        edgesync_events_fire(w->event_cb, w->event_cb_ctx, EDGESYNC_EVENT_DEAD_LETTER, msg);
        break;

    case EDGESYNC_DELIVERY_RETRYABLE_FAILURE:
    default:
        handle_retryable_failure(w, msg, esp_timer_get_time());
        break;
    }
}

static void worker_task(void *arg)
{
    edgesync_worker_t *w = arg;

    while (!w->stop_requested) {
        edgesync_message_t msg;
        esp_err_t err = edgesync_queue_claim_next(w->queue, esp_timer_get_time(), &msg);

        if (err == EDGESYNC_ERR_NOT_FOUND) {
            /* Nothing eligible right now; sleep until woken by a publish or
             * the idle poll interval elapses (to notice matured retries). */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_EDGESYNC_WORKER_IDLE_POLL_MS));
            continue;
        }
        if (err == EDGESYNC_ERR_STORAGE_CORRUPT) {
            /* The backend already dead-lettered the message while trying to
             * read it back; just surface the event. */
            edgesync_events_fire(w->event_cb, w->event_cb_ctx, EDGESYNC_EVENT_DEAD_LETTER, &msg);
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "claim_next failed: %s", esp_err_to_name(err));
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        int status_code = 0;
        edgesync_delivery_result_t result = w->transport_ops->deliver(w->transport_ctx, &msg, &status_code);
        handle_delivery_result(w, &msg, result);
    }

    xSemaphoreGive(w->stopped_sem);
    vTaskDelete(NULL);
}

esp_err_t edgesync_worker_start(const edgesync_worker_config_t *config, edgesync_worker_t **out_worker)
{
    if (config == NULL || config->queue == NULL || config->transport_ops == NULL || out_worker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    edgesync_worker_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return ESP_ERR_NO_MEM;
    }

    w->queue = config->queue;
    w->transport_ops = config->transport_ops;
    w->transport_ctx = config->transport_ctx;
    w->retry_cfg = config->retry_cfg;
    w->event_cb = config->event_cb;
    w->event_cb_ctx = config->event_cb_ctx;

    w->stopped_sem = xSemaphoreCreateBinary();
    if (w->stopped_sem == NULL) {
        free(w);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(worker_task, "edgesync", config->task_stack_size, w,
                                             config->task_priority, &w->task, config->task_core_id);
    if (ok != pdPASS) {
        vSemaphoreDelete(w->stopped_sem);
        free(w);
        return ESP_ERR_NO_MEM;
    }

    edgesync_queue_set_wake_cb(w->queue, wake_cb, w);

    *out_worker = w;
    return ESP_OK;
}

void edgesync_worker_stop(edgesync_worker_t *worker)
{
    if (worker == NULL) {
        return;
    }

    edgesync_queue_set_wake_cb(worker->queue, NULL, NULL);
    worker->stop_requested = true;
    xTaskNotifyGive(worker->task);

    xSemaphoreTake(worker->stopped_sem, portMAX_DELAY);
    vSemaphoreDelete(worker->stopped_sem);
    free(worker);
}
