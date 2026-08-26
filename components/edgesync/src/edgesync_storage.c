#include "edgesync_storage.h"
#include "edgesync_storage_flashq.h"
#include "edgesync_storage_nvs.h"

esp_err_t edgesync_storage_create(const edgesync_config_t *config,
                                   uint32_t effective_max_messages,
                                   uint32_t effective_max_message_size,
                                   const edgesync_storage_ops_t **out_ops,
                                   void **out_ctx)
{
    switch (config->storage) {
    case EDGESYNC_STORAGE_FLASHQ: {
        edgesync_flashq_config_t fq_cfg = {
            .partition_label = config->storage_config.partition_label,
            .max_messages = effective_max_messages,
            .max_message_size = effective_max_message_size,
        };
        return edgesync_flashq_create(&fq_cfg, out_ops, out_ctx);
    }
    case EDGESYNC_STORAGE_NVS: {
        edgesync_nvs_storage_config_t nvs_cfg = {
            .nvs_namespace = config->storage_config.nvs_namespace,
            .max_messages = effective_max_messages,
            .max_message_size = effective_max_message_size,
        };
        return edgesync_nvs_storage_create(&nvs_cfg, out_ops, out_ctx);
    }
    case EDGESYNC_STORAGE_CUSTOM:
        if (config->storage_ops == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        *out_ops = config->storage_ops;
        *out_ctx = config->storage_ctx;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

void edgesync_storage_destroy(const edgesync_config_t *config,
                               const edgesync_storage_ops_t *ops, void *ctx)
{
    if (ops && ops->close) {
        ops->close(ctx);
    }
    switch (config->storage) {
    case EDGESYNC_STORAGE_FLASHQ:
        edgesync_flashq_destroy(ctx);
        break;
    case EDGESYNC_STORAGE_NVS:
        edgesync_nvs_storage_destroy(ctx);
        break;
    default:
        break; /* Custom backend owns its own ctx lifetime. */
    }
}
