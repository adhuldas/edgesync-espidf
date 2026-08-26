/**
 * @file edgesync_events.h
 * @brief Optional application event callback.
 */
#pragma once

#include "edgesync_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EDGESYNC_EVENT_QUEUED,      /**< Message durably accepted into the queue. */
    EDGESYNC_EVENT_DELIVERED,   /**< Message acknowledged by the destination. */
    EDGESYNC_EVENT_RETRY,       /**< Delivery attempt failed (retryable); a retry was scheduled. */
    EDGESYNC_EVENT_FAILED,      /**< Delivery attempt failed (informational, fired alongside RETRY/DEAD_LETTER). */
    EDGESYNC_EVENT_QUEUE_FULL,  /**< A publish was rejected or caused a drop due to queue limits. */
    EDGESYNC_EVENT_DEAD_LETTER, /**< Message moved to the dead-letter state; it will not be retried automatically. */
} edgesync_event_id_t;

/**
 * @brief Application event callback.
 *
 * Invoked from the EdgeSync synchronization task's context, outside of any
 * internal lock. Implementations MUST be fast and non-blocking: they must
 * not call back into EdgeSync APIs that touch the queue synchronously in a
 * way that blocks, perform long I/O, or sleep for long periods. A slow
 * callback directly delays retries and connectivity handling for every
 * queued message.
 *
 * `message` is a borrowed pointer valid only for the duration of the
 * callback; copy any fields you need to retain.
 */
typedef void (*edgesync_event_cb_t)(edgesync_event_id_t event,
                                     const edgesync_message_t *message,
                                     void *user_ctx);

#ifdef __cplusplus
}
#endif
