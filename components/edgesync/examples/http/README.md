# EdgeSync: HTTP(S) Example

Connects to Wi-Fi, then publishes a small JSON payload every 5 seconds over
EdgeSync's built-in HTTP(S) transport. Because `edgesync_publish()` never
blocks on the network, messages queue up durably even before Wi-Fi finishes
connecting, or if it drops later - EdgeSync drains them as connectivity
allows.

## Configure

```
idf.py menuconfig
```

Under **EdgeSync HTTP Example**, set:

- `Wi-Fi SSID` / `Wi-Fi password`
- `EdgeSync HTTP(S) ingest URL` - defaults to `https://httpbin.org/post`,
  which echoes the request back and is only usable for connectivity
  testing, not a real ingest endpoint.

## Run it

```
idf.py set-target esp32
idf.py -p PORT flash monitor
```

## Notes

- TLS certificate verification is on by default for `https://` URLs via the
  ESP-IDF certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`, set in
  `sdkconfig.defaults`).
- See
  [TRANSPORTS.md](https://github.com/adhuldas/edgesync-espidf/blob/main/components/edgesync/TRANSPORTS.md)
  for the full HTTP transport configuration surface (headers, auth,
  idempotency key, status classification).
