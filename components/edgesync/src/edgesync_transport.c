#include "edgesync_transport.h"
#include "edgesync_http.h"

esp_err_t edgesync_transport_create(const edgesync_config_t *config,
                                     const edgesync_transport_ops_t **out_ops, void **out_ctx)
{
    if (config->transport_ops != NULL) {
        *out_ops = config->transport_ops;
        *out_ctx = config->transport_ctx;
        return ESP_OK;
    }
    return edgesync_http_transport_create(&config->http, out_ops, out_ctx);
}

void edgesync_transport_destroy(const edgesync_config_t *config,
                                 const edgesync_transport_ops_t *ops, void *ctx)
{
    if (ops && ops->close) {
        ops->close(ctx);
    }
    if (config->transport_ops == NULL) {
        edgesync_http_transport_destroy(ctx);
    }
}
