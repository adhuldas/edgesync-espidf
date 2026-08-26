# Reliability

## Durability guarantee

`edgesync_publish()` returns `ESP_OK` **only** after the message has been
written to the configured storage backend and, for the flash-queue backend,
verified by reading it back (see [STORAGE.md](STORAGE.md)). From that point
on the message survives task crash, reboot, and power loss: it will be
delivered (or explicitly dead-lettered) even if the device restarts before
the network is ever reachable.

What EdgeSync does **not** guarantee:

- **Exactly-once delivery.** A crash between the destination accepting a
  message and EdgeSync durably recording `DELIVERED` will cause a redelivery
  on restart. Destinations should treat delivery as **at-least-once** and
  deduplicate using the message id (see the `Idempotency-Key` header on the
  built-in HTTP transport).
- **Ordering across priorities.** Higher-priority messages are always
  claimed first; ordering is only preserved among messages of the same
  priority (oldest eligible first).
- **Delivery within a time bound.** With `retry_max_attempts == 0` (the
  default) a message retries forever if the destination never accepts it.

## Message state machine

```
                 publish()
                    |
                    v
   +-----------> PENDING  <---------------------+
   |                |                            |
   |  retry with    | claim_next() (worker)      | schedule_retry(),
   |  backoff       v                            | not dead-lettered
   |            IN_FLIGHT ----deliver() fails---->+
   |                |
   |          deliver() succeeds
   |                |
   |                v
   |           DELIVERED (awaiting compaction/GC)
   |
   +-- retries exhausted, or permanent failure, or storage corruption
                    |
                    v
               DEAD_LETTER (kept for inspection; never retried automatically)
```

- **PENDING** — waiting to be claimed. New messages start here.
- **IN_FLIGHT** — claimed by the worker task; a delivery attempt is (or was)
  in progress. `attempt_count` is incremented durably *before* the attempt is
  made, so a crash mid-delivery cannot under-count attempts.
- **DELIVERED** — acknowledged by the destination. Space is reclaimed on the
  backend's next compaction/GC pass; it does not occupy a "live" slot.
- **DEAD_LETTER** — will not be retried automatically. Kept (subject to the
  backend's capacity) so the application can inspect `last_error` and decide
  whether to re-publish.

### Crash recovery

On `edgesync_init()`, the storage backend scans its backing store and
transitions any message still marked **IN_FLIGHT** back to **PENDING** — the
device cannot know whether the in-progress attempt reached the destination,
so it is retried. Combined with idempotent destinations (see above), this
makes the queue safe to resume after any restart.

## Retry / backoff

Delay before attempt *N* (N ≥ 2; the first attempt is always immediate):

```
delay = min(retry_max_ms, retry_initial_ms * retry_multiplier^(N-2))
```

with **equal jitter** applied by default (`retry_jitter = true`): the final
delay is `delay/2 + random(0, delay/2)`, which halves the worst case while
still avoiding synchronized retries across many devices (thundering herd).

- `retry_max_attempts == 0` (default): retry forever.
- `retry_max_attempts == N`: dead-letter after the Nth failed attempt.
- A transport's `deliver()` classifies each failure as **retryable** (network
  error, timeout, 5xx/429/408) or **permanent** (4xx client error); permanent
  failures skip straight to dead-letter regardless of `retry_max_attempts`.

## Overflow policies

Applied when the queue is at `max_queue_size` and a new message is published:

| Policy | Behavior |
|---|---|
| `EDGESYNC_OVERFLOW_REJECT_NEW` (default) | `edgesync_publish()` returns `EDGESYNC_ERR_QUEUE_FULL`; nothing is queued or dropped. |
| `EDGESYNC_OVERFLOW_DROP_OLDEST` | The oldest `PENDING` message is dropped to make room. |
| `EDGESYNC_OVERFLOW_DROP_LOWEST_PRIORITY` | The lowest-priority (then oldest) `PENDING` message is dropped. |

Only `PENDING` messages are eligible to be dropped — `IN_FLIGHT` and
`DEAD_LETTER` messages are never silently discarded. `EDGESYNC_EVENT_QUEUE_FULL`
fires for both the dropped message (if any) and, if no message could be
dropped, the rejected publish.

## Thread safety

`edgesync_publish()` and `edgesync_get_stats()` may be called concurrently
from any number of tasks once `edgesync_start()` has returned; all storage
access is serialized internally. `edgesync_init()` / `edgesync_start()` /
`edgesync_stop()` / `edgesync_deinit()` are lifecycle calls and must not be
called concurrently with each other, or from the event callback.
