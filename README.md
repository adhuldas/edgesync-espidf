# edgesync-espidf

Reliable store-and-forward data delivery for ESP-IDF: a durable, crash-safe
flash queue with automatic retry/backoff, delivered over HTTP(S), MQTT, or
any pluggable transport.

The component itself lives in [`components/edgesync`](components/edgesync),
along with its full documentation:

- [README](components/edgesync/README.md) — overview, quick start, security notes.
- [RELIABILITY.md](components/edgesync/RELIABILITY.md) — durability guarantee, message states, retry/backoff.
- [STORAGE.md](components/edgesync/STORAGE.md) — on-flash format, compaction, and backend selection.
- [TRANSPORTS.md](components/edgesync/TRANSPORTS.md) — the transport interface and writing a custom one.
- [CONTRIBUTING.md](components/edgesync/CONTRIBUTING.md) — running the host and on-target test suites.
- [CHANGELOG.md](components/edgesync/CHANGELOG.md)

## Examples

- [`examples/basic`](components/edgesync/examples/basic) — no network stack, logs each message.
- [`examples/http`](components/edgesync/examples/http) — delivery over HTTP(S).
- [`examples/mqtt`](components/edgesync/examples/mqtt) — delivery over MQTT.

## Installing

```
idf.py add-dependency "adhuldas/edgesync"
```

or add it directly as a `git` submodule / component under `components/`.
