/**
 * @file edgesync_storage_nvs.h
 * @brief Constructor for the NVS-backed storage backend.
 *
 * See STORAGE.md for why this backend is only appropriate for small,
 * low-frequency queues: every state transition (claim/retry/dead-letter)
 * rewrites the entire message blob (payload included), and each durable
 * write requires an nvs_commit(). The flash-queue backend
 * (EDGESYNC_STORAGE_FLASHQ) should be preferred for telemetry-rate traffic.
 */
#pragma once

#include "edgesync/edgesync_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *nvs_namespace;   /**< NULL -> "edgesync". Uses the default NVS partition. */
    uint32_t max_messages;       /**< Keep small (tens, not hundreds) - see STORAGE.md. */
    uint32_t max_message_size;
} edgesync_nvs_storage_config_t;

esp_err_t edgesync_nvs_storage_create(const edgesync_nvs_storage_config_t *config,
                                       const edgesync_storage_ops_t **out_ops, void **out_ctx);

void edgesync_nvs_storage_destroy(void *ctx);

#ifdef __cplusplus
}
#endif
