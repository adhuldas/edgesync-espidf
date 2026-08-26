/**
 * @file edgesync_mqtt.h
 * @brief Built-in MQTT transport (esp-mqtt-based).
 *
 * Not wired in automatically like the HTTP transport - call
 * edgesync_mqtt_transport_create() before edgesync_init() and assign the
 * result to edgesync_config_t::transport_ops / transport_ctx. See
 * TRANSPORTS.md for QoS/ack semantics and Wi-Fi/PPP(GSM) interchangeability.
 */
#pragma once

#include "edgesync_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the built-in MQTT transport.
 *
 * Opens an esp-mqtt client handle but does not connect yet - the connection
 * is established when edgesync_init() calls the transport's init(), and
 * maintained (with automatic reconnect) for the lifetime of the transport.
 *
 * @param config Must outlive the transport; not copied by pointer fields.
 */
esp_err_t edgesync_mqtt_transport_create(const edgesync_mqtt_transport_config_t *config,
                                          const edgesync_transport_ops_t **out_ops, void **out_ctx);

/** Stop the client and release resources. Call after edgesync_deinit(). */
void edgesync_mqtt_transport_destroy(void *ctx);

#ifdef __cplusplus
}
#endif
