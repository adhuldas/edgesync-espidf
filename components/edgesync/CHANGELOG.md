# Changelog

## 0.2.0

- Built-in MQTT transport (`edgesync_mqtt`), usable as a drop-in alternative
  to the HTTP(S) transport, including over a cellular (GSM) PPP link.
- New `examples/mqtt` example.
- New `examples/basic` and `examples/http` READMEs.

## 0.1.0

- Initial implementation: durable publish/retry/deliver pipeline, crash-safe
  log-structured flash-queue storage backend, NVS storage backend, built-in
  HTTP(S) transport, pluggable storage/transport interfaces, event callback,
  and stats snapshot.
