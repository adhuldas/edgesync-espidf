/**
 * @file edgesync_storage_flashq.h
 * @brief Constructor for the log-structured flash-queue storage backend.
 *
 * See STORAGE.md for the on-flash format (two ping-pong regions, append-only
 * records, CRC-guarded torn-write detection, bit-clearing region status).
 */
#pragma once

#include "edgesync/edgesync_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *partition_label; /**< NULL -> "edgesync". Must be a `data` type partition. */
    uint32_t max_messages;       /**< In-memory index capacity; bounds RAM usage. */
    uint32_t max_message_size;   /**< Max payload bytes per message. */
} edgesync_flashq_config_t;

/**
 * @brief Create a flash-queue storage backend instance.
 *
 * Allocates internal state (index array + scratch buffers) but does not
 * touch flash - call ops->init() then ops->recover() before use.
 */
esp_err_t edgesync_flashq_create(const edgesync_flashq_config_t *config,
                                  const edgesync_storage_ops_t **out_ops,
                                  void **out_ctx);

/** Free everything allocated by edgesync_flashq_create(). Call after ops->close(). */
void edgesync_flashq_destroy(void *ctx);

#ifdef __cplusplus
}
#endif
