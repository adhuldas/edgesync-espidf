/**
 * @file edgesync.h
 * @brief EdgeSync public API - reliable store-and-forward data delivery for ESP-IDF.
 *
 * See README.md for an overview and RELIABILITY.md for the delivery
 * guarantees this component provides.
 *
 * Thread safety: edgesync_publish() and edgesync_get_stats() are safe to
 * call concurrently from any number of FreeRTOS tasks once edgesync_start()
 * has returned. edgesync_init()/edgesync_start()/edgesync_stop()/
 * edgesync_deinit() are lifecycle calls and must not be called concurrently
 * with each other or from an EdgeSync event callback.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "edgesync_types.h"
#include "edgesync_storage.h"
#include "edgesync_transport.h"
#include "edgesync_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Error codes (dedicated range; see esp_err.h conventions)
 * ---------------------------------------------------------------------- */
#define ESP_ERR_EDGESYNC_BASE           0x8A00

#define EDGESYNC_ERR_QUEUE_FULL         (ESP_ERR_EDGESYNC_BASE + 1) /**< Overflow policy is REJECT_NEW and the queue is full. */
#define EDGESYNC_ERR_STORAGE            (ESP_ERR_EDGESYNC_BASE + 2) /**< Underlying storage I/O failure. */
#define EDGESYNC_ERR_SERIALIZATION      (ESP_ERR_EDGESYNC_BASE + 3) /**< Record encode/decode failure (corruption). */
#define EDGESYNC_ERR_NOT_INITIALIZED    (ESP_ERR_EDGESYNC_BASE + 4) /**< Handle not initialized or already deinitialized. */
#define EDGESYNC_ERR_ALREADY_STARTED    (ESP_ERR_EDGESYNC_BASE + 5) /**< edgesync_start() called twice. */
#define EDGESYNC_ERR_INVALID_STATE      (ESP_ERR_EDGESYNC_BASE + 6) /**< Operation not valid in the current lifecycle state. */
#define EDGESYNC_ERR_MESSAGE_TOO_LARGE  (ESP_ERR_EDGESYNC_BASE + 7) /**< Payload or destination exceeds configured limits. */
#define EDGESYNC_ERR_STORAGE_CORRUPT    (ESP_ERR_EDGESYNC_BASE + 8) /**< Storage failed recovery/validation. */
#define EDGESYNC_ERR_NOT_FOUND          (ESP_ERR_EDGESYNC_BASE + 9) /**< No eligible message / id not found. */
#define EDGESYNC_ERR_TRANSPORT          (ESP_ERR_EDGESYNC_BASE + 10) /**< Transport misconfigured (e.g. missing URL). */

/**
 * @brief EdgeSync instance configuration.
 *
 * Use EDGESYNC_DEFAULT_CONFIG() to get sane, Kconfig-derived defaults and
 * then override only the fields you care about.
 */
typedef struct {
    /* --- Storage --- */
    edgesync_storage_type_t storage;
    edgesync_storage_config_t storage_config;
    const edgesync_storage_ops_t *storage_ops; /**< Required iff storage == EDGESYNC_STORAGE_CUSTOM. */
    void *storage_ctx;                          /**< Context passed to storage_ops calls (custom backend only). */

    /* --- Queue limits --- */
    uint32_t max_queue_size;     /**< Max message count. 0 -> CONFIG_EDGESYNC_MAX_QUEUE_SIZE. */
    uint32_t max_storage_bytes;  /**< Soft cap on total payload bytes retained. 0 -> unlimited (bounded by partition size). */
    uint32_t max_message_size;   /**< Max payload bytes per message. 0 -> CONFIG_EDGESYNC_MAX_MESSAGE_SIZE. */
    edgesync_overflow_policy_t overflow_policy;

    /* --- Retry / backoff (exponential with optional jitter) --- */
    uint32_t retry_initial_ms;   /**< 0 -> CONFIG_EDGESYNC_DEFAULT_RETRY_INITIAL_MS. */
    uint32_t retry_max_ms;       /**< 0 -> CONFIG_EDGESYNC_DEFAULT_RETRY_MAX_MS. */
    uint32_t retry_multiplier;   /**< 0 -> 2. */
    bool retry_jitter;           /**< Default true. */
    uint32_t retry_max_attempts; /**< 0 -> unlimited retries (message stays PENDING/retrying forever). */

    /* --- Transport: leave transport_ops NULL to use the built-in HTTP(S) transport with `http`. --- */
    const edgesync_transport_ops_t *transport_ops;
    void *transport_ctx;
    edgesync_http_transport_config_t http;

    /* --- FreeRTOS task --- */
    uint32_t task_stack_size;    /**< 0 -> CONFIG_EDGESYNC_TASK_STACK_SIZE. */
    UBaseType_t task_priority;   /**< 0 -> CONFIG_EDGESYNC_TASK_PRIORITY. */
    BaseType_t task_core_id;     /**< tskNO_AFFINITY by default. */

    /* --- Events --- */
    edgesync_event_cb_t event_cb; /**< Optional. See edgesync_events.h for callback constraints. */
    void *event_cb_ctx;
} edgesync_config_t;

/** Zero-initialized default configuration relying on Kconfig defaults; override fields as needed. */
#define EDGESYNC_DEFAULT_CONFIG()                     \
    {                                                  \
        .storage = EDGESYNC_STORAGE_FLASHQ,           \
        .storage_config = {0},                        \
        .storage_ops = NULL,                          \
        .storage_ctx = NULL,                          \
        .max_queue_size = 0,                          \
        .max_storage_bytes = 0,                       \
        .max_message_size = 0,                        \
        .overflow_policy = EDGESYNC_OVERFLOW_REJECT_NEW, \
        .retry_initial_ms = 0,                        \
        .retry_max_ms = 0,                             \
        .retry_multiplier = 0,                        \
        .retry_jitter = true,                          \
        .retry_max_attempts = 0,                        \
        .transport_ops = NULL,                          \
        .transport_ctx = NULL,                           \
        .http = {0},                                     \
        .task_stack_size = 0,                             \
        .task_priority = 0,                                \
        .task_core_id = tskNO_AFFINITY,                     \
        .event_cb = NULL,                                    \
        .event_cb_ctx = NULL,                                 \
    }

/** Per-publish options. Pass NULL to edgesync_publish() to use defaults. */
typedef struct {
    edgesync_message_id_t message_id; /**< 0 (default) -> auto-generate. Caller-supplied IDs must be caller-unique. */
    edgesync_priority_t priority;     /**< Default EDGESYNC_PRIORITY_NORMAL. */
} edgesync_publish_options_t;

/**
 * @brief Create an EdgeSync instance: initializes storage and runs crash recovery.
 *
 * Does not start the background worker or accept publishes yet (see
 * edgesync_start()). Safe to call once per handle; call edgesync_deinit()
 * before re-initializing.
 *
 * @param config Configuration; copied internally, does not need to outlive the call
 *               EXCEPT for pointers it contains (url, header strings, custom
 *               storage/transport ops) which must remain valid for the
 *               lifetime of the handle.
 * @param out_handle Receives the new handle on success.
 */
esp_err_t edgesync_init(const edgesync_config_t *config, edgesync_handle_t *out_handle);

/**
 * @brief Start the background synchronization task and connectivity monitoring.
 *
 * After this returns ESP_OK, edgesync_publish() will actively be drained to
 * the configured transport as connectivity allows.
 */
esp_err_t edgesync_start(edgesync_handle_t handle);

/**
 * @brief Durably enqueue a message for delivery.
 *
 * Returns ESP_OK only after the message has been durably persisted
 * according to the configured storage backend - see the top-level
 * durability guarantee in README.md/RELIABILITY.md. Does not block on
 * network availability; safe to call while disconnected.
 *
 * @param destination Logical destination/topic, e.g. "telemetry". Max
 *                     EDGESYNC_MAX_DESTINATION_LEN bytes (NUL-terminated C string).
 * @param data Payload bytes. Copied internally; caller retains ownership of `data` and
 *             may free/reuse it immediately after this call returns (success or failure).
 * @param data_len Payload length in bytes. Must not exceed the configured max_message_size.
 * @param options Optional; NULL for defaults.
 *
 * @return ESP_OK on durable acceptance.
 * @return EDGESYNC_ERR_QUEUE_FULL if overflow_policy is REJECT_NEW and the queue is full.
 * @return EDGESYNC_ERR_MESSAGE_TOO_LARGE if data_len/destination exceed configured limits.
 * @return EDGESYNC_ERR_NOT_INITIALIZED if called before edgesync_start().
 */
esp_err_t edgesync_publish(edgesync_handle_t handle,
                            const char *destination,
                            const void *data,
                            size_t data_len,
                            const edgesync_publish_options_t *options);

/**
 * @brief Stop the background worker gracefully.
 *
 * Waits for any in-progress delivery attempt to finish or time out, then
 * tears down the worker task and connectivity handlers. The queue and its
 * contents are left untouched on flash. May be called from any task except
 * the EdgeSync worker task itself (i.e. not from an event callback).
 */
esp_err_t edgesync_stop(edgesync_handle_t handle);

/**
 * @brief Release all resources associated with the handle.
 *
 * The handle must have been stopped first (or never started). Does not
 * erase persisted queue data - a subsequent edgesync_init() with the same
 * storage configuration will recover it.
 */
esp_err_t edgesync_deinit(edgesync_handle_t handle);

/** Populate `stats` with a point-in-time snapshot of queue/delivery health. */
esp_err_t edgesync_get_stats(edgesync_handle_t handle, edgesync_stats_t *stats);

#ifdef __cplusplus
}
#endif
