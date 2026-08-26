# EdgeSync: Basic Example

Minimal example with no network stack required. Demonstrates the durable
queue on its own, using a trivial "transport" that just logs each message
instead of sending it anywhere.

Publishes a small JSON payload every 2 seconds and logs the queue stats
(`edgesync_get_stats()`) and lifecycle events (queued / delivered / retry /
dead-letter) as they happen.

## Run it

```
idf.py set-target esp32
idf.py -p PORT flash monitor
```

No configuration needed - it builds and runs standalone.

## Where to go next

Swap the `logging_transport` in `main.c` for the built-in HTTP(S) transport
(see the `http` example) or the built-in MQTT transport (see the `mqtt`
example) once you have real connectivity, or plug in your own
`edgesync_transport_ops_t` - see
[TRANSPORTS.md](https://github.com/adhuldas/edgesync-espidf/blob/main/components/edgesync/TRANSPORTS.md).
