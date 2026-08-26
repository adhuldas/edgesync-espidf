/**
 * @file edgesync_events.h (internal)
 * @brief Single choke point for firing the optional application event callback.
 */
#pragma once

#include "edgesync/edgesync_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/** No-op if `cb` is NULL. */
void edgesync_events_fire(edgesync_event_cb_t cb, void *cb_ctx,
                           edgesync_event_id_t event, const edgesync_message_t *message);

#ifdef __cplusplus
}
#endif
