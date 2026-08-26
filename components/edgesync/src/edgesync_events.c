#include "edgesync_events.h"

void edgesync_events_fire(edgesync_event_cb_t cb, void *cb_ctx,
                           edgesync_event_id_t event, const edgesync_message_t *message)
{
    if (cb != NULL) {
        cb(event, message, cb_ctx);
    }
}
