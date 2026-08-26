# Contributing

## Layout

```
include/edgesync/   Public API (installed headers)
src/                 Implementation (internal headers + .c files)
test_host/           Host-native unit tests for pure/hardware-independent modules
test/                On-target (ESP-IDF Unity) integration tests
examples/            Standalone example projects
```

## Host tests

Modules with no ESP-IDF/FreeRTOS dependency (`edgesync_crc32`,
`edgesync_retry`, `edgesync_flashq_format`) are unit-tested as plain C,
compiled and run directly on the host — no target, QEMU, or Wi-Fi required.
This is deliberate: encode/decode, CRC, and backoff-math bugs should be
caught in seconds during development, not by flashing a board.

```sh
cd test_host
make test
```

When adding logic to one of these modules (or a new one), prefer keeping it
free of `esp_*`/FreeRTOS calls specifically so it can be tested this way —
inject inputs like randomness or the current time as parameters instead of
calling `esp_random()`/`esp_timer_get_time()` internally (see
`edgesync_retry_compute_delay_ms()` for the pattern).

## On-target tests

Everything that touches flash, NVS, FreeRTOS tasks, or the network lives in
`test/` as an ESP-IDF Unity test app:

```sh
cd test
idf.py set-target esp32   # or your target
idf.py build flash monitor
```

## Style

- No dynamic behavior beyond what's configured: don't add silent fallbacks,
  retries, or timeouts that aren't part of the documented contract.
- Every `esp_err_t`-returning function documents its non-`ESP_OK` return
  values at the call site or in the header.
- Storage backend changes must preserve the crash-consistency contract in
  `edgesync_storage_ops_t` (see [STORAGE.md](STORAGE.md)) — when in doubt,
  add a host test that simulates a torn write.
