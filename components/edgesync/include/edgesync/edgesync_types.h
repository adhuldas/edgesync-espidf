/**
 * @file edgesync_types.h
 * @brief Public data types shared across the EdgeSync API.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to an EdgeSync instance. */
typedef struct edgesync_ctx *edgesync_handle_t;

/**
 * @brief 64-bit message identifier.
 *
 * Generated from a hardware RNG (esp_fill_random) at publish time unless the
 * caller supplies one. 64 bits of randomness gives a collision probability
 * that is negligible for the lifetime message volume of a single device
 * (birthday bound: ~2^32 messages before a 50% collision chance), while
 * staying far more compact than a 128-bit/36-character UUID string -
 * important for flash wear and RAM footprint on constrained targets.
 *
 * The ID is stored inside the durable record itself, so it is stable across
 * restarts, and is suitable for use as an HTTP `Idempotency-Key` (hex
 * encoded, 16 characters).
 */
typedef uint64_t edgesync_message_id_t;

/** Value reserved to mean "no id supplied / not found". */
#define EDGESYNC_MESSAGE_ID_INVALID ((edgesync_message_id_t)0)

/** Delivery priority. Higher-priority messages are claimed for delivery first. */
typedef enum {
    EDGESYNC_PRIORITY_LOW    = 0,
    EDGESYNC_PRIORITY_NORMAL = 1,
    EDGESYNC_PRIORITY_HIGH   = 2,
} edgesync_priority_t;

/** Lifecycle state of a queued message. See RELIABILITY.md for the full state machine. */
typedef enum {
    EDGESYNC_MSG_PENDING     = 0, /**< Waiting to be claimed for delivery. */
    EDGESYNC_MSG_IN_FLIGHT   = 1, /**< Claimed by the worker, delivery in progress. */
    EDGESYNC_MSG_DELIVERED   = 2, /**< Acknowledged by the destination. Awaiting GC/compaction. */
    EDGESYNC_MSG_DEAD_LETTER = 3, /**< Permanently failed or exhausted retries. Kept for inspection. */
} edgesync_message_state_t;

/** Overflow behavior when the queue reaches its configured limits. */
typedef enum {
    EDGESYNC_OVERFLOW_REJECT_NEW = 0,          /**< Default. edgesync_publish() returns EDGESYNC_ERR_QUEUE_FULL. */
    EDGESYNC_OVERFLOW_DROP_OLDEST = 1,          /**< Drop the oldest PENDING message to make room. */
    EDGESYNC_OVERFLOW_DROP_LOWEST_PRIORITY = 2, /**< Drop the lowest-priority (then oldest) PENDING message. */
} edgesync_overflow_policy_t;

/** Maximum destination string length (excluding NUL), configurable via Kconfig. */
#ifndef EDGESYNC_MAX_DESTINATION_LEN
#define EDGESYNC_MAX_DESTINATION_LEN 63
#endif

/**
 * @brief In-memory view of a queued message.
 *
 * Instances of this struct are produced by the storage layer (e.g. from
 * edgesync_storage_ops_t::claim_next) and consumed by the worker/transport.
 *
 * Ownership: `payload` is a borrowed pointer into a scratch buffer owned by
 * the storage backend. It is only valid until the next call into the same
 * storage instance (from the same task) - callers must not retain it across
 * calls or after the owning storage/queue lock is released.
 */
typedef struct {
    edgesync_message_id_t id;
    char destination[EDGESYNC_MAX_DESTINATION_LEN + 1];
    edgesync_priority_t priority;
    edgesync_message_state_t state;
    uint32_t attempt_count;
    int64_t created_at_us;      /**< esp_timer_get_time() at creation (monotonic, not wall clock). */
    int64_t last_attempt_at_us;
    int64_t next_retry_at_us;
    esp_err_t last_error;
    uint32_t payload_len;
    const void *payload;        /**< Borrowed; see ownership note above. May be NULL if payload_len == 0. */
} edgesync_message_t;

/** Runtime statistics. Counters marked "since boot" are not persisted. */
typedef struct {
    uint32_t pending_messages;
    uint32_t in_flight_messages;
    uint32_t retrying_messages;
    uint32_t dead_letter_messages;
    uint32_t delivered_awaiting_gc;   /**< Delivered messages still occupying flash until compaction. */

    uint64_t bytes_queued;            /**< Sum of payload bytes currently retained (all non-purged states). */

    uint64_t successful_deliveries;   /**< Since boot. */
    uint64_t failed_deliveries;       /**< Since boot (retryable + permanent). */

    uint32_t queue_capacity;          /**< Effective max_queue_size in force. */
    uint32_t storage_bytes_used;
    uint32_t storage_bytes_capacity;

    uint32_t compactions_total;       /**< Since boot. */
} edgesync_stats_t;

#ifdef __cplusplus
}
#endif
