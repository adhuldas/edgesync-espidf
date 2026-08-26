/**
 * @file edgesync_storage.h (internal)
 * @brief Backend selection glue: picks/creates the configured storage backend.
 */
#pragma once

#include "edgesync/edgesync.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Instantiate the storage backend selected by `config`.
 *
 * On success, *out_ops and *out_ctx are ready for init()+recover() to be called.
 */
esp_err_t edgesync_storage_create(const edgesync_config_t *config,
                                   uint32_t effective_max_messages,
                                   uint32_t effective_max_message_size,
                                   const edgesync_storage_ops_t **out_ops,
                                   void **out_ctx);

/** Tear down a backend created by edgesync_storage_create() (calls close() then frees ctx). */
void edgesync_storage_destroy(const edgesync_config_t *config,
                               const edgesync_storage_ops_t *ops, void *ctx);

#ifdef __cplusplus
}
#endif
