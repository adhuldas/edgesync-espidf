/**
 * @file edgesync_transport.h
 * @brief Transport abstraction and built-in HTTP(S) transport configuration.
 *
 * EdgeSync core has no knowledge of HTTP, MQTT, or any other wire protocol.
 * It drives an `edgesync_transport_ops_t` and reacts only to the
 * classification returned by `deliver()`. This keeps the retry/queue logic
 * transport-agnostic; see TRANSPORTS.md.
 */
#pragma once

#include "edgesync_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Outcome of a single delivery attempt. The transport classifies; the core decides what to do next. */
typedef enum {
    EDGESYNC_DELIVERY_SUCCESS = 0,           /**< Destination accepted the message; safe to mark DELIVERED. */
    EDGESYNC_DELIVERY_RETRYABLE_FAILURE = 1, /**< Transient failure; core will schedule a retry with backoff. */
    EDGESYNC_DELIVERY_PERMANENT_FAILURE = 2, /**< Destination rejected the message; core will dead-letter it. */
} edgesync_delivery_result_t;

/**
 * @brief Transport operations table.
 *
 * `ctx` is the opaque pointer supplied via edgesync_config_t::transport_ctx.
 * `deliver()` is called from the EdgeSync worker task only (never
 * concurrently), so implementations do not need to be reentrant, but they
 * must not block indefinitely - honor timeouts internally.
 */
typedef struct edgesync_transport_ops {
    /** One-time setup (e.g. open a client handle). May be NULL. */
    esp_err_t (*init)(void *ctx);

    /**
     * Attempt to deliver a single message. Must not free or retain `message`
     * or its payload beyond the call.
     *
     * @param out_status_code Optional: protocol-specific status (e.g. HTTP code) for logging/stats. May be NULL.
     */
    edgesync_delivery_result_t (*deliver)(void *ctx,
                                           const edgesync_message_t *message,
                                           int *out_status_code);

    /** Release resources. May be NULL. */
    esp_err_t (*close)(void *ctx);
} edgesync_transport_ops_t;

/**
 * @brief Configuration for the built-in HTTP(S) transport.
 *
 * Used when edgesync_config_t::transport_ops is left NULL.
 */
typedef struct {
    const char *url;                 /**< Base URL, e.g. "https://api.example.com/ingest". Required. */
    bool no_append_destination;      /**< Default false: "/<destination>" is appended to the URL path.
                                           Set true to POST every destination to the exact same URL
                                           (the destination is still sent via the X-EdgeSync-Destination header). */

    uint32_t timeout_ms;             /**< Per-attempt timeout. 0 -> Kconfig default. */

    const char *content_type;        /**< NULL -> "application/octet-stream". */
    const char *idempotency_header;  /**< NULL -> default "Idempotency-Key". Pass "" to disable the header entirely. */

    const char *auth_header_name;    /**< Optional, e.g. "Authorization". Not logged. */
    const char *auth_header_value;   /**< Optional. Not logged. */

    const char *const *extra_header_names;   /**< Optional array of additional static header names. */
    const char *const *extra_header_values;  /**< Parallel array of values. */
    size_t extra_header_count;

    bool use_cert_bundle;            /**< Default true: attach ESP-IDF's bundled CA store via esp_crt_bundle_attach. */
    const char *cert_pem;            /**< Optional PEM CA cert; overrides use_cert_bundle when non-NULL. */
    bool skip_cert_common_name_check; /**< Default false. Strongly discouraged; see SECURITY notes in README. */

    /**
     * Optional override for HTTP status -> retryability classification.
     * Return -1 to fall back to the built-in default classification
     * (see TRANSPORTS.md). May be NULL.
     */
    edgesync_delivery_result_t (*classify_status)(int http_status);
} edgesync_http_transport_config_t;

/**
 * @brief Configuration for the built-in MQTT transport.
 *
 * Unlike the HTTP transport, this one is not wired in automatically - the
 * MQTT client keeps a persistent connection and session, so create it
 * explicitly with edgesync_mqtt_transport_create() (edgesync_mqtt.h) and
 * hand the resulting ops/ctx to edgesync_config_t::transport_ops /
 * transport_ctx. It only talks to the standard IP netif, so the same
 * configuration works unchanged over Wi-Fi, Ethernet, or a PPP link brought
 * up over a cellular modem (e.g. via esp_modem) - swapping the network
 * underneath requires no transport changes; see TRANSPORTS.md.
 */
typedef struct {
    const char *broker_uri;          /**< e.g. "mqtt://host:1883" or "mqtts://host:8883". Required. */
    const char *topic;                /**< Base topic. Required. */
    bool no_append_destination;      /**< Default false: "/<destination>" is appended to the topic.
                                           Set true to publish every destination to the exact same topic. */

    const char *client_id;           /**< NULL -> esp-mqtt default (derived from MAC address). */
    const char *username;            /**< Optional. Not logged. */
    const char *password;            /**< Optional. Not logged. */

    int qos;                         /**< 0, 1, or 2. Any other value (e.g. -1) -> Kconfig default. */
    bool retain;                     /**< Set the MQTT retain flag on published messages. */

    uint32_t timeout_ms;             /**< Per-publish wait for local hand-off (QoS 0) or broker ack (QoS 1/2).
                                           0 -> Kconfig default. */

    bool use_cert_bundle;            /**< Default true: attach ESP-IDF's bundled CA store via esp_crt_bundle_attach
                                           for mqtts:// URIs. */
    const char *cert_pem;            /**< Optional PEM CA cert; overrides use_cert_bundle when non-NULL. */
    bool skip_cert_common_name_check; /**< Default false. Strongly discouraged; see SECURITY notes in README. */

    const char *client_cert_pem;     /**< Optional client certificate for mutual TLS. */
    const char *client_key_pem;      /**< Optional client private key for mutual TLS. */
} edgesync_mqtt_transport_config_t;

#ifdef __cplusplus
}
#endif
