# Storage backends

EdgeSync ships two storage backends, selected via `edgesync_config_t::storage`,
plus a hook (`EDGESYNC_STORAGE_CUSTOM`) for supplying your own via
`edgesync_storage_ops_t` (see `edgesync_storage.h`).

| | `EDGESYNC_STORAGE_FLASHQ` (default) | `EDGESYNC_STORAGE_NVS` |
|---|---|---|
| Backing store | Dedicated `data` partition | Default NVS partition |
| Best for | High-frequency telemetry | Small (tens of messages), low-frequency queues |
| Retry/state-change cost | Small append-only record | Full message rewrite + `nvs_commit()` |
| Compaction | Background, crash-safe, automatic | None (no reclaim of deleted-key space) |

## `EDGESYNC_STORAGE_FLASHQ`: log-structured flash queue

### Partition

Requires a dedicated `data`-type partition (default label `"edgesync"`,
override via `storage_config.partition_label`), at least 4 flash sectors
(16 KiB) and sector-aligned:

```csv
# Name,     Type, SubType, Offset,  Size
edgesync,   data, 0x40,    ,        64K
```

The subtype byte (`0x40` above) is an arbitrary custom subtype — anything not
already reserved by ESP-IDF is fine, since EdgeSync locates the partition by
label, not subtype.

### On-flash format

The partition is split into two equal-size **regions**; exactly one is
**ACTIVE** at a time, the other kept erased as spare. Each region starts with
a header sector (status + generation counter), followed by an append-only
log of small, CRC-32-protected records:

- **MESSAGE** — written once at `enqueue()`: id, destination, priority,
  payload, creation time.
- **UPDATE** — written for every later state change (claimed → `IN_FLIGHT`,
  delivered, retried, dead-lettered, removed). Retries never rewrite the
  payload, so redelivery attempts are cheap in flash-write terms.

Records are never rewritten in place — only appended — which is what makes
each individual write crash-safe: a torn write during power loss leaves a
record that fails its CRC check and is simply ignored during recovery,
never one that is read back as a different, valid-looking record.

### Region header transitions

The region header's status byte only ever **clears bits**, which is the one
transition NOR flash can make without a full sector erase:

```
EMPTY (0xFF) -> COMPACTING (0x07) -> ACTIVE (0x03) -> STALE (0x01)
```

### Compaction

Triggered when the active region is low on free space or has accumulated
many UPDATE records relative to its capacity. Compaction copies only
still-relevant messages (not `DELIVERED`/`REMOVED`) into the spare region,
flips it to `ACTIVE`, marks the old region `STALE`, then erases it. Every
step is safe to interrupt: if a crash lands mid-copy, recovery finds the old
region still `ACTIVE` and restarts compaction from scratch; if a crash lands
after the flip but before the old region is erased, recovery finds two
`ACTIVE`-looking regions and picks the one with the higher generation
counter.

### Recovery

On `edgesync_init()`, the active region is scanned to rebuild the in-memory
index from the MESSAGE/UPDATE log. Any message still `IN_FLIGHT` is
requeued as `PENDING` (see [RELIABILITY.md](RELIABILITY.md)). If a message's
payload fails its CRC check when it is finally claimed for delivery, it is
dead-lettered rather than delivered with corrupted data.

### Sizing

`max_messages` (from `max_queue_size`) bounds a fixed-size in-memory index
(RAM cost, independent of partition size) — each queued message occupies one
slot regardless of state. `max_message_size` bounds the largest single
payload and therefore the scratch buffers used for encode/verify/claim.
Choose the partition size so that `max_messages` full-size messages fit with
room to spare for compaction headroom (at least one sector, plus the UPDATE
records accumulated between compactions).

## `EDGESYNC_STORAGE_NVS`

Stores each message as one NVS blob key in the given namespace (default
`"edgesync"`) within the device's default NVS partition — no partition
changes required, but with real limitations:

- **Every** state change (claim, retry, dead-letter) rewrites the *entire*
  message blob, including the payload, and calls `nvs_commit()`. There is no
  append-only fast path like the flash-queue backend.
- NVS keys are limited to 15 characters, which bounds how large
  `max_messages` can practically be before key management gets awkward.
- No compaction: NVS itself reclaims space from deleted keys, but at its own
  pace and wear-leveling policy, not EdgeSync's.

Appropriate for small, low-frequency queues (tens of messages, configuration
or command-acknowledgement style traffic) where flash-partition setup is
undesirable. For telemetry-rate publishing, use `EDGESYNC_STORAGE_FLASHQ`.

## Writing a custom backend

Implement `edgesync_storage_ops_t` (`edgesync_storage.h`) and set
`storage = EDGESYNC_STORAGE_CUSTOM`, `storage_ops`, and `storage_ctx` in
`edgesync_config_t`. The key invariant every backend must uphold: `enqueue()`
must never leave a partially-written record that could later be read back as
valid (see the "crash consistency" contract on `edgesync_storage_ops_t`).
