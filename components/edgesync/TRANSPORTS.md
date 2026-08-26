# Transports

EdgeSync core has no knowledge of HTTP, MQTT, or any other wire protocol. It
drives an `edgesync_transport_ops_t` (`edgesync_transport.h`) and reacts only
to the `edgesync_delivery_result_t` returned by `deliver()`. This keeps the
retry/queue logic transport-agnostic — see [RELIABILITY.md](RELIABILITY.md)
for what happens to a message after each outcome.

## The interface

```c
typedef struct edgesync_transport_ops {
    esp_err_t (*init)(void *ctx);                     /* optional, once at edgesync_init() */
    edgesync_delivery_result_t (*deliver)(void *ctx,
                                          const edgesync_message_t *message,
                                          int *out_status_code);
    esp_err_t (*close)(void *ctx);                     /* optional, at edgesync_deinit() */
} edgesync_transport_ops_t;
```

- `deliver()` is called from the EdgeSync worker task only — never
  concurrently — so implementations do not need to be reentrant, but they
  must not block indefinitely; honor a timeout internally. A slow or hung
  `deliver()` stalls every other queued message, including higher-priority
  ones.
- `message->payload` is a borrowed pointer, valid only for the duration of
  the call. Do not retain it or free it.
- `out_status_code` is optional (may be `NULL`) and is purely for
  logging/stats — return a protocol-specific code such as an HTTP status if
  one is meaningful, or leave it untouched otherwise.
- Return exactly one of:
  - `EDGESYNC_DELIVERY_SUCCESS` — the destination accepted the message; it
    will be marked `DELIVERED` and never sent again.
  - `EDGESYNC_DELIVERY_RETRYABLE_FAILURE` — a transient failure (timeout,
    connection refused, 5xx, ...); a retry is scheduled with backoff.
  - `EDGESYNC_DELIVERY_PERMANENT_FAILURE` — the destination will never
    accept this message (4xx-style rejection); it is dead-lettered
    immediately, bypassing `retry_max_attempts`.

Getting the retryable/permanent classification right matters: classifying a
malformed-request rejection as retryable wastes bandwidth and battery
retrying something that will never succeed; classifying a transient network
blip as permanent loses data unnecessarily.

## Built-in HTTP(S) transport

Used automatically whenever `edgesync_config_t::transport_ops` is left
`NULL`, configured via `edgesync_config_t::http`
(`edgesync_http_transport_config_t` in `edgesync_transport.h`):

- POSTs each message to `"<url>/<destination>"` (or exactly `url`, with the
  destination only sent via the `X-EdgeSync-Destination` header, if
  `no_append_destination` is set).
- TLS verification is on by default for `https://` URLs via the ESP-IDF
  certificate bundle; see the "Security" section of [README.md](README.md).
- Sends the message id as the `Idempotency-Key` header by default (16 lowercase
  hex characters) — disable with `idempotency_header = ""`.
- Default status → retryability classification: 2xx → success; 400/401/403/
  404/422 → permanent; 408/429/5xx → retryable; anything else → retryable if
  ≥ 500, otherwise permanent. Override with `classify_status` if your API
  uses different conventions (e.g. a 200 body indicating an application-level
  error).
- No response at all (DNS failure, connection refused, TLS handshake
  failure, timeout) is always retryable.

## Writing a custom transport

Set `transport_ops` and (optionally) `transport_ctx` in `edgesync_config_t`;
`http` is then ignored. A minimal example that always reports success (for
testing) or a fixed-endpoint MQTT publish:

```c
static edgesync_delivery_result_t my_deliver(void *ctx, const edgesync_message_t *msg, int *status)
{
    my_client_t *client = ctx;
    int rc = my_client_publish(client, msg->destination, msg->payload, msg->payload_len);
    if (status) *status = rc;
    if (rc == MY_OK) return EDGESYNC_DELIVERY_SUCCESS;
    if (rc == MY_ERR_REJECTED) return EDGESYNC_DELIVERY_PERMANENT_FAILURE;
    return EDGESYNC_DELIVERY_RETRYABLE_FAILURE;
}

static const edgesync_transport_ops_t my_ops = {
    .deliver = my_deliver,
};

edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
config.transport_ops = &my_ops;
config.transport_ctx = my_client;
```
