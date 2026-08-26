/**
 * @file edgesync_transport.h (internal)
 * @brief Transport backend selection glue.
 */
#pragma once

#include "edgesync/edgesync.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t edgesync_transport_create(const edgesync_config_t *config,
                                     const edgesync_transport_ops_t **out_ops, void **out_ctx);

void edgesync_transport_destroy(const edgesync_config_t *config,
                                 const edgesync_transport_ops_t *ops, void *ctx);

#ifdef __cplusplus
}
#endif
