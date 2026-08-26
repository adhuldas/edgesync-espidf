/**
 * @file edgesync_worker.h (internal)
 * @brief FreeRTOS task that drains the queue to the configured transport.
 */
#pragma once

#include "freertos/FreeRTOS.h"

#include "edgesync/edgesync.h"
#include "edgesync_queue.h"
#include "edgesync_retry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct edgesync_worker edgesync_worker_t;

typedef struct {
    edgesync_queue_t *queue;
    const edgesync_transport_ops_t *transport_ops;
    void *transport_ctx;
    edgesync_retry_config_t retry_cfg; /**< Already defaulted (see edgesync_retry_apply_defaults()). */
    edgesync_event_cb_t event_cb;
    void *event_cb_ctx;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    BaseType_t task_core_id;
} edgesync_worker_config_t;

/** Registers itself as the queue's wake callback; call edgesync_worker_stop() to unregister and tear down. */
esp_err_t edgesync_worker_start(const edgesync_worker_config_t *config, edgesync_worker_t **out_worker);

/**
 * Signal the worker to stop, wait for any in-progress delivery attempt to
 * finish (or its transport timeout to elapse), then free it. Blocking.
 */
void edgesync_worker_stop(edgesync_worker_t *worker);

#ifdef __cplusplus
}
#endif
