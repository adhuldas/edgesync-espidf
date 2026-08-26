/**
 * @file edgesync_http.h
 * @brief Built-in HTTP(S) transport (esp_http_client-based).
 */
#pragma once

#include "edgesync/edgesync_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t edgesync_http_transport_create(const edgesync_http_transport_config_t *config,
                                          const edgesync_transport_ops_t **out_ops, void **out_ctx);

void edgesync_http_transport_destroy(void *ctx);

#ifdef __cplusplus
}
#endif
