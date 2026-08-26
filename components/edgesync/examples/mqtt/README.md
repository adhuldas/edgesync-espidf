# EdgeSync: MQTT Example

Connects to Wi-Fi, then publishes a small JSON payload every 5 seconds over
EdgeSync's built-in MQTT transport. Because `edgesync_publish()` never
blocks on the network, messages queue up durably even before the network
finishes connecting, or if it drops later - EdgeSync drains them as
connectivity allows.

## Configure

```
idf.py menuconfig
```

Under **EdgeSync MQTT Example**, set:

- `Wi-Fi SSID` / `Wi-Fi password`
- `MQTT broker URI` - defaults to the public, unauthenticated
  `mqtt://broker.hivemq.com:1883`, which is only usable for connectivity
  testing (it's world-readable), not real data.
- `EdgeSync MQTT base topic` - each publish goes to
  `"<this>/<destination>"`.

## Run it

```
idf.py set-target esp32
idf.py -p PORT flash monitor
```

## Cellular (GSM) instead of Wi-Fi

This transport only talks to whatever IP network is already up - it has no
idea whether that's Wi-Fi, Ethernet, or a PPP link brought up over a
cellular modem. To run over GSM instead, replace `wifi_init_station()` in
`main.c` with a PPP bring-up using
[`esp_modem`](https://components.espressif.com/components/espressif/esp_modem)
(or your own AT-command sequence); the transport config, topic layout, QoS
handling, and retry behavior are all unchanged.

## Notes

- TLS certificate verification is on by default for `mqtts://` URIs via the
  ESP-IDF certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`, set in
  `sdkconfig.defaults`).
- See
  [TRANSPORTS.md](https://github.com/adhuldas/edgesync-espidf/blob/main/components/edgesync/TRANSPORTS.md)
  for the full MQTT transport configuration surface (QoS, ack timeout,
  credentials, mutual TLS) and why it never returns a permanent-failure
  classification.
