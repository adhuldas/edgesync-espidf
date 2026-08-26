# EdgeSync

Reliable store-and-forward data delivery for ESP-IDF.

EdgeSync durably queues messages on flash, then drains them to a destination
(HTTP(S) by default, or any transport you supply) with automatic retry and
exponential backoff. It is designed for devices that publish data — telemetry,
logs, events — while intermittently or unreliably connected: `edgesync_publish()`
never blocks on the network, and a message is only ever reported as accepted
once it has survived a power loss.

See [RELIABILITY.md](RELIABILITY.md) for the exact durability guarantee and
message state machine, [STORAGE.md](STORAGE.md) for the on-flash format, and
[TRANSPORTS.md](TRANSPORTS.md) for writing a custom transport.

## Features

- **Durable queue** — messages survive reboot, crash, and power loss.
- **Crash-safe flash backend** — a log-structured, ping-pong flash queue
  (`EDGESYNC_STORAGE_FLASHQ`, the default) designed for high-frequency
  telemetry, with an NVS-backed alternative for small, low-frequency queues.
- **Automatic retry** — exponential backoff with jitter, configurable limits,
  and dead-lettering of messages that exhaust their retries or are
  permanently rejected.
- **Pluggable transport** — a built-in HTTP(S) client (TLS verified by
  default via the ESP-IDF certificate bundle), or bring your own
  (MQTT, CoAP, BLE, ...).
- **Backpressure policies** — reject new messages, or drop the oldest /
  lowest-priority pending message when the queue is full.
- **Observable** — an optional event callback (queued / delivered / retry /
  failed / dead-lettered / queue-full) and a point-in-time stats snapshot.

## Quick start

Add a dedicated `data` partition for the flash queue to your `partitions.csv`
(at least 4 sectors / 16 KiB; see [STORAGE.md](STORAGE.md)):

```csv
# Name,     Type, SubType, Offset,  Size
edgesync,   data, 0x40,    ,        64K
```

```c
#include "edgesync/edgesync.h"

static void on_event(edgesync_event_id_t event, const edgesync_message_t *msg, void *ctx)
{
    ESP_LOGI("app", "edgesync event %d for message %016llx", event, (unsigned long long)msg->id);
}

void app_main(void)
{
    edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
    config.http.url = "https://api.example.com/ingest";
    config.event_cb = on_event;

    edgesync_handle_t handle;
    ESP_ERROR_CHECK(edgesync_init(&config, &handle));
    ESP_ERROR_CHECK(edgesync_start(handle));

    const char *payload = "{\"temp\": 21.5}";
    esp_err_t err = edgesync_publish(handle, "telemetry", payload, strlen(payload), NULL);
    if (err == ESP_OK) {
        // durably queued; EdgeSync will deliver it as connectivity allows
    }
}
```

## Security

- TLS certificate verification is **on by default** for `https://` URLs via
  the ESP-IDF certificate bundle (`esp_crt_bundle_attach`). Supply
  `http.cert_pem` to pin a specific CA instead, or set
  `http.skip_cert_common_name_check` only if you understand the risk (it
  disables hostname verification, not certificate chain validation).
- `http.auth_header_value` and other credentials are never logged by
  EdgeSync. They are held in RAM only (not persisted with the queued
  message) and passed to `esp_http_client` per request.
- The message id is exposed as the `Idempotency-Key` HTTP header by default
  so that a server-side retry-safe endpoint can deduplicate redelivery after
  a device reboot mid-delivery.

## Documentation

- [RELIABILITY.md](RELIABILITY.md) — durability guarantee, message states, retry/backoff.
- [STORAGE.md](STORAGE.md) — on-flash format, compaction, and backend selection.
- [TRANSPORTS.md](TRANSPORTS.md) — the transport interface and writing a custom one.
- [CONTRIBUTING.md](CONTRIBUTING.md) — running the host and on-target test suites.
- [CHANGELOG.md](CHANGELOG.md)
