/**
 * @file edgesync_queue.h (internal)
 * @brief Mutex-guarded façade over a storage backend.
 *
 * Owns the single lock that serializes access to the storage backend between
 * the caller(s) of edgesync_publish() and the EdgeSync worker task, applies
 * the configured overflow policy, tracks since-boot delivery counters, and
 * fires application events outside the lock.
 */
#pragma once

#include "edgesync/edgesync.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edgesync_queue edgesync_queue_t;

typedef struct {
    const edgesync_storage_ops_t *storage_ops;
    void *storage_ctx;
    edgesync_overflow_policy_t overflow_policy;
    edgesync_event_cb_t event_cb;
    void *event_cb_ctx;
} edgesync_queue_config_t;

esp_err_t edgesync_queue_create(const edgesync_queue_config_t *config, edgesync_queue_t **out_queue);

/** Runs storage init() + recover(). Must be called once before any other queue operation. */
esp_err_t edgesync_queue_recover(edgesync_queue_t *queue);

/** Durably enqueue `message`, applying the overflow policy if the backend is full. */
esp_err_t edgesync_queue_publish(edgesync_queue_t *queue, const edgesync_message_t *message);

esp_err_t edgesync_queue_claim_next(edgesync_queue_t *queue, int64_t now_us, edgesync_message_t *out_message);

/** Increments the since-boot successful-delivery counter on success. */
esp_err_t edgesync_queue_mark_delivered(edgesync_queue_t *queue, edgesync_message_id_t id);

/** Increments the since-boot failed-delivery counter on success. */
esp_err_t edgesync_queue_schedule_retry(edgesync_queue_t *queue, edgesync_message_id_t id,
                                         int64_t next_retry_at_us, bool dead_letter, esp_err_t last_error);

esp_err_t edgesync_queue_get_stats(edgesync_queue_t *queue, edgesync_stats_t *stats);

/**
 * Register a callback invoked (from the publishing task, after the lock is
 * released) whenever a message is durably enqueued, so the worker can wake
 * immediately instead of waiting out its idle poll interval. Pass cb == NULL
 * to unregister.
 */
void edgesync_queue_set_wake_cb(edgesync_queue_t *queue, void (*wake_cb)(void *ctx), void *wake_ctx);

void edgesync_queue_destroy(edgesync_queue_t *queue);

#ifdef __cplusplus
}
#endif
