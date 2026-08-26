/**
 * @file edgesync_storage.h
 * @brief Storage backend abstraction.
 *
 * This is an advanced/extension interface. Most applications never touch it
 * directly - select a backend via edgesync_config_t::storage instead. It is
 * exposed so that alternative backends (e.g. FATFS/SD card) can be added
 * without modifying queue/worker logic. See STORAGE.md for the on-flash
 * format used by the built-in flash-queue backend and why NVS is not used
 * as the primary high-frequency queue.
 */
#pragma once

#include "edgesync_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Storage backend operations table.
 *
 * All calls are made while the queue layer holds its internal mutex, EXCEPT
 * that the queue layer never calls a storage op while blocked on flash
 * hardware for longer than necessary - implementations must be bounded and
 * must not perform network I/O. `ctx` is backend-private state allocated by
 * the backend's constructor.
 *
 * Error contract: any function may return ESP_FAIL / EDGESYNC_ERR_STORAGE on
 * I/O failure. Implementations must guarantee that a failed `enqueue` never
 * leaves a partially-written, ambiguous record that could later be read back
 * as valid (see STORAGE.md, "crash consistency").
 */
typedef struct edgesync_storage_ops {
    /** One-time initialization (mount/open backing store). Does NOT scan/recover. */
    esp_err_t (*init)(void *ctx);

    /**
     * Scan the backing store, rebuild the in-memory index, resolve any
     * interrupted writes/compactions, and transition any IN_FLIGHT message
     * back to PENDING. Must be called once after init(), before any other
     * operation, and must be idempotent if called again.
     */
    esp_err_t (*recover)(void *ctx);

    /**
     * Durably append a new message with state PENDING. Only returns ESP_OK
     * once the record (and its integrity check) has been verified on the
     * backing store - i.e. once this returns ESP_OK the caller may treat the
     * message as accepted per EdgeSync's durability guarantee.
     */
    esp_err_t (*enqueue)(void *ctx, const edgesync_message_t *message);

    /**
     * Select the next eligible message (highest priority, then oldest
     * `next_retry_at_us` that is <= now, respecting state == PENDING),
     * durably transition it to IN_FLIGHT, and copy it into *out_message.
     * Returns EDGESYNC_ERR_NOT_FOUND if no message is currently eligible.
     */
    esp_err_t (*claim_next)(void *ctx, int64_t now_us, edgesync_message_t *out_message);

    /** Durably mark a message DELIVERED. Space is reclaimed on the next compaction. */
    esp_err_t (*mark_delivered)(void *ctx, edgesync_message_id_t id);

    /**
     * Durably record a failed attempt and either reschedule (state=PENDING,
     * next_retry_at_us = next_retry_time_us) or dead-letter the message
     * (state=DEAD_LETTER) depending on `dead_letter`.
     */
    esp_err_t (*schedule_retry)(void *ctx, edgesync_message_id_t id,
                                 int64_t next_retry_time_us, bool dead_letter,
                                 esp_err_t last_error);

    /** Remove a message immediately regardless of state (used by overflow policies). */
    esp_err_t (*remove)(void *ctx, edgesync_message_id_t id);

    /**
     * Select which message the queue layer's overflow policy should drop
     * next (does not remove it - the queue layer calls remove() afterwards
     * and fires EDGESYNC_EVENT_QUEUE_FULL). Only PENDING messages are
     * eligible. Returns EDGESYNC_ERR_NOT_FOUND if none are eligible (e.g.
     * every slot is IN_FLIGHT or DEAD_LETTER).
     */
    esp_err_t (*find_drop_candidate)(void *ctx, edgesync_overflow_policy_t policy,
                                      edgesync_message_id_t *out_id);

    /** Populate backend-observable stats fields (queue counts, bytes used/capacity). */
    esp_err_t (*get_stats)(void *ctx, edgesync_stats_t *stats);

    /** Release resources. */
    esp_err_t (*close)(void *ctx);
} edgesync_storage_ops_t;

/** Backend selector for edgesync_config_t::storage. */
typedef enum {
    /**
     * Log-structured, wear-aware flash queue on a dedicated partition.
     * Recommended default; suitable for high-frequency telemetry. See
     * STORAGE.md.
     */
    EDGESYNC_STORAGE_FLASHQ = 0,

    /**
     * NVS-backed queue. Simple, reuses the existing NVS partition, but is
     * only appropriate for small (tens of messages), low-frequency queues -
     * see STORAGE.md for the documented limitations (key length, blob
     * overhead, index rewrite wear, no built-in compaction).
     */
    EDGESYNC_STORAGE_NVS = 1,

    /** Application-supplied backend via edgesync_config_t::storage_ops. */
    EDGESYNC_STORAGE_CUSTOM = 2,
} edgesync_storage_type_t;

/** Backend-specific configuration. */
typedef struct {
    const char *partition_label; /**< EDGESYNC_STORAGE_FLASHQ: custom `data` partition label. Default "edgesync". */
    const char *nvs_namespace;   /**< EDGESYNC_STORAGE_NVS: namespace within the default NVS partition. Default "edgesync". */
} edgesync_storage_config_t;

#ifdef __cplusplus
}
#endif
