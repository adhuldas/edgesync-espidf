/**
 * @file edgesync_storage_flashq.c
 * @brief Log-structured, ping-pong flash-queue storage backend.
 *
 * See STORAGE.md for the full design rationale. Summary:
 *  - A dedicated `data` partition is split into two equal-size regions.
 *  - Exactly one region is ACTIVE at a time; the other is kept erased/spare.
 *  - New state is always *appended* to the active region as a small,
 *    self-describing, CRC-protected record - never rewritten in place.
 *    A full message write (MESSAGE) happens once at enqueue; every later
 *    state change (claimed, delivered, retried, dead-lettered, dropped) is
 *    a small UPDATE record referencing the same message id, so retries do
 *    not repeatedly burn flash writing the payload again.
 *  - Region header status bytes only ever clear bits (EMPTY 0xFF ->
 *    COMPACTING 0x07 -> ACTIVE 0x03 -> STALE 0x01), which is the only
 *    transition NOR flash can make without a full sector erase.
 *  - Compaction copies still-relevant messages into the spare region,
 *    appends an END marker, flips it to ACTIVE, marks the old region STALE,
 *    then erases it. Recovery always prefers whichever region is ACTIVE
 *    (highest generation if both are, which only happens if a crash landed
 *    exactly between flipping the new region ACTIVE and marking the old one
 *    STALE) - this makes every step of compaction safe to interrupt.
 */
#include <string.h>
#include <stdlib.h>

#include "esp_partition.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "edgesync/edgesync.h"
#include "edgesync_storage_flashq.h"
#include "edgesync_flashq_format.h"

static const char *TAG = "EdgeSync";

#define EDGESYNC_FLASHQ_SECTOR_SIZE 4096u
#define EDGESYNC_FLASHQ_NUM_REGIONS 2
/* Keep at least this much headroom free before it is safe to assume a
 * subsequent write (of up to one full-size record) will succeed without
 * triggering compaction again immediately. */
#define EDGESYNC_FLASHQ_RESERVE_BYTES EDGESYNC_FLASHQ_SECTOR_SIZE

typedef struct {
    edgesync_message_id_t id;
    uint8_t state;      /* edgesync_flashq_state_t */
    uint8_t priority;   /* edgesync_priority_t */
    uint16_t destination_len;
    uint32_t attempt_count;
    uint32_t payload_len;
    uint32_t message_offset; /* offset of the MESSAGE record within the active region */
    int64_t created_at_us;
    int64_t last_attempt_at_us;
    int64_t next_retry_at_us;
    int32_t last_error;
} flashq_index_entry_t;

typedef struct {
    const esp_partition_t *partition;
    size_t sectors_per_region;
    size_t region_size;   /* bytes, whole region incl. header sector */
    size_t data_start;    /* = EDGESYNC_FLASHQ_SECTOR_SIZE, offset of first record within a region */
    size_t data_capacity; /* = region_size - data_start */

    int active_region;
    uint32_t active_generation;
    size_t cursor;      /* next write offset within the active region (absolute-in-region, >= data_start) */
    uint32_t next_seq;

    flashq_index_entry_t *index;
    size_t index_count;
    size_t index_capacity;

    uint32_t max_message_size;
    size_t scratch_size; /* sized for the largest possible record */
    uint8_t *write_scratch;
    uint8_t *verify_scratch;
    uint8_t *claim_scratch;

    uint32_t compactions_total;
    uint32_t awaiting_gc_count;   /* DELIVERED + REMOVED messages still occupying flash */
    uint32_t updates_since_compaction;

    SemaphoreHandle_t lock;
} edgesync_flashq_ctx_t;

/* ------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

static inline size_t region_offset(const edgesync_flashq_ctx_t *ctx, int region)
{
    return (size_t)region * ctx->region_size;
}

static inline size_t max_record_size(const edgesync_flashq_ctx_t *ctx)
{
    return edgesync_flashq_record_total_size((uint16_t)EDGESYNC_MAX_DESTINATION_LEN, ctx->max_message_size);
}

static bool bytes_all_ff(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static esp_err_t region_header_read(edgesync_flashq_ctx_t *ctx, int region,
                                     edgesync_flashq_region_hdr_t *out_hdr, bool *out_blank)
{
    uint8_t raw[EDGESYNC_FLASHQ_REGION_HDR_SIZE];
    esp_err_t err = esp_partition_read(ctx->partition, region_offset(ctx, region), raw, sizeof(raw));
    if (err != ESP_OK) {
        return EDGESYNC_ERR_STORAGE;
    }
    *out_blank = bytes_all_ff(raw, sizeof(raw));
    memcpy(out_hdr, raw, sizeof(raw));
    return ESP_OK;
}

static esp_err_t partition_write_verify(edgesync_flashq_ctx_t *ctx, size_t abs_offset,
                                         const void *data, size_t len)
{
    esp_err_t err = esp_partition_write(ctx->partition, abs_offset, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flash write failed at 0x%x (%d)", (unsigned)abs_offset, err);
        return EDGESYNC_ERR_STORAGE;
    }
    err = esp_partition_read(ctx->partition, abs_offset, ctx->verify_scratch, len);
    if (err != ESP_OK || memcmp(ctx->verify_scratch, data, len) != 0) {
        ESP_LOGE(TAG, "flash write verify failed at 0x%x", (unsigned)abs_offset);
        return EDGESYNC_ERR_STORAGE;
    }
    return ESP_OK;
}

static esp_err_t erase_region(edgesync_flashq_ctx_t *ctx, int region)
{
    ESP_LOGI(TAG, "erasing region %d (%u bytes)", region, (unsigned)ctx->region_size);
    esp_err_t err = esp_partition_erase_range(ctx->partition, region_offset(ctx, region), ctx->region_size);
    return (err == ESP_OK) ? ESP_OK : EDGESYNC_ERR_STORAGE;
}

static esp_err_t ensure_region_erased(edgesync_flashq_ctx_t *ctx, int region)
{
    edgesync_flashq_region_hdr_t hdr;
    bool blank = false;
    esp_err_t err = region_header_read(ctx, region, &hdr, &blank);
    if (err != ESP_OK || !blank) {
        return erase_region(ctx, region);
    }
    return ESP_OK;
}

static flashq_index_entry_t *find_entry(edgesync_flashq_ctx_t *ctx, edgesync_message_id_t id)
{
    for (size_t i = 0; i < ctx->index_count; i++) {
        if (ctx->index[i].id == id) {
            return &ctx->index[i];
        }
    }
    return NULL;
}

static void remove_entry_at(edgesync_flashq_ctx_t *ctx, size_t pos)
{
    ctx->index[pos] = ctx->index[ctx->index_count - 1];
    ctx->index_count--;
}

/* ------------------------------------------------------------------------
 * Append path (shared by enqueue / claim_next / mark_delivered / schedule_retry / remove)
 * ---------------------------------------------------------------------- */

static esp_err_t compact_locked(edgesync_flashq_ctx_t *ctx);

static esp_err_t ensure_space_locked(edgesync_flashq_ctx_t *ctx, size_t needed_len)
{
    size_t used = ctx->cursor - ctx->data_start;
    size_t free_bytes = ctx->data_capacity - used;

    bool low_space = free_bytes < needed_len + EDGESYNC_FLASHQ_RESERVE_BYTES;
    bool too_churned = ctx->updates_since_compaction > (uint32_t)(ctx->index_capacity * 4 + 16);

    if (low_space || too_churned) {
        esp_err_t err = compact_locked(ctx);
        if (err != ESP_OK) {
            return err;
        }
        used = ctx->cursor - ctx->data_start;
        free_bytes = ctx->data_capacity - used;
    }

    if (free_bytes < needed_len) {
        ESP_LOGE(TAG, "flash queue region full even after compaction (need %u, have %u)",
                 (unsigned)needed_len, (unsigned)free_bytes);
        return EDGESYNC_ERR_STORAGE;
    }
    return ESP_OK;
}

static esp_err_t append_locked(edgesync_flashq_ctx_t *ctx, const uint8_t *buf, size_t len)
{
    size_t abs_offset = region_offset(ctx, ctx->active_region) + ctx->cursor;
    esp_err_t err = partition_write_verify(ctx, abs_offset, buf, len);
    if (err != ESP_OK) {
        return err;
    }
    ctx->cursor += len;
    ctx->next_seq++;
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Compaction
 * ---------------------------------------------------------------------- */

static esp_err_t compact_locked(edgesync_flashq_ctx_t *ctx)
{
    int target = 1 - ctx->active_region;
    int source = ctx->active_region;

    ESP_LOGI(TAG, "compaction starting: %u live message(s), source region %d -> target region %d",
             (unsigned)ctx->index_count, source, target);

    esp_err_t err = ensure_region_erased(ctx, target);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t new_generation = ctx->active_generation + 1;
    edgesync_flashq_region_hdr_t hdr = {
        .magic = EDGESYNC_FLASHQ_HDR_MAGIC,
        .generation = new_generation,
        .status = EDGESYNC_FLASHQ_STATUS_COMPACTING,
        .reserved = {0},
    };
    err = partition_write_verify(ctx, region_offset(ctx, target), &hdr, sizeof(hdr));
    if (err != ESP_OK) {
        return err;
    }

    size_t new_cursor = ctx->data_start;
    uint32_t seq = ctx->next_seq;

    for (size_t i = 0; i < ctx->index_count; i++) {
        flashq_index_entry_t *e = &ctx->index[i];

        size_t body_len = (size_t)e->destination_len + e->payload_len;
        size_t old_abs = region_offset(ctx, source) + e->message_offset;
        err = esp_partition_read(ctx->partition, old_abs, ctx->verify_scratch,
                                  EDGESYNC_FLASHQ_HDR_SIZE + body_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "compaction: failed to read source record for id %016llx",
                     (unsigned long long)e->id);
            return EDGESYNC_ERR_STORAGE;
        }

        char destination[EDGESYNC_MAX_DESTINATION_LEN + 1] = {0};
        memcpy(destination, ctx->verify_scratch + EDGESYNC_FLASHQ_HDR_SIZE, e->destination_len);
        const uint8_t *payload = ctx->verify_scratch + EDGESYNC_FLASHQ_HDR_SIZE + e->destination_len;

        size_t enc_len = 0;
        if (!edgesync_flashq_encode_message_full(ctx->write_scratch, ctx->scratch_size, &enc_len,
                                                  e->id, destination, (edgesync_priority_t)e->priority,
                                                  (edgesync_flashq_state_t)e->state, e->attempt_count,
                                                  e->created_at_us, e->last_attempt_at_us, e->next_retry_at_us,
                                                  e->last_error, payload, e->payload_len, seq)) {
            ESP_LOGE(TAG, "compaction: failed to re-encode id %016llx", (unsigned long long)e->id);
            return EDGESYNC_ERR_SERIALIZATION;
        }

        err = partition_write_verify(ctx, region_offset(ctx, target) + new_cursor, ctx->write_scratch, enc_len);
        if (err != ESP_OK) {
            return err;
        }

        e->message_offset = (uint32_t)new_cursor;
        new_cursor += enc_len;
        seq++;
    }

    size_t end_len = 0;
    edgesync_flashq_encode_end(ctx->write_scratch, ctx->scratch_size, &end_len, seq);
    err = partition_write_verify(ctx, region_offset(ctx, target) + new_cursor, ctx->write_scratch, end_len);
    if (err != ESP_OK) {
        return err;
    }
    new_cursor += end_len;
    seq++;

    uint8_t active_status = EDGESYNC_FLASHQ_STATUS_ACTIVE;
    err = partition_write_verify(ctx, region_offset(ctx, target) + offsetof(edgesync_flashq_region_hdr_t, status),
                                  &active_status, sizeof(active_status));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t stale_status = EDGESYNC_FLASHQ_STATUS_STALE;
    err = partition_write_verify(ctx, region_offset(ctx, source) + offsetof(edgesync_flashq_region_hdr_t, status),
                                  &stale_status, sizeof(stale_status));
    if (err != ESP_OK) {
        /* Non-fatal: the old region is already superseded, worst case it is
         * cleaned up on the next boot's recovery pass or next compaction. */
        ESP_LOGW(TAG, "failed to mark old region STALE (non-fatal)");
    }

    (void)ensure_region_erased(ctx, source); /* best-effort, reclaimed lazily on failure */

    ctx->active_region = target;
    ctx->active_generation = new_generation;
    ctx->cursor = new_cursor;
    ctx->next_seq = seq;
    ctx->compactions_total++;
    ctx->updates_since_compaction = 0;
    ctx->awaiting_gc_count = 0;

    ESP_LOGI(TAG, "compaction complete: region %d active (gen %u), %u bytes used",
             target, (unsigned)new_generation, (unsigned)(new_cursor - ctx->data_start));
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Recovery
 * ---------------------------------------------------------------------- */

static esp_err_t append_state_update_locked(edgesync_flashq_ctx_t *ctx, flashq_index_entry_t *e,
                                             edgesync_flashq_state_t new_state)
{
    size_t needed = edgesync_flashq_record_total_size(0, 0);
    esp_err_t err = ensure_space_locked(ctx, needed);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = 0;
    edgesync_flashq_encode_update(ctx->write_scratch, ctx->scratch_size, &len, e->id, new_state,
                                   e->attempt_count, e->last_attempt_at_us, e->next_retry_at_us,
                                   e->last_error, ctx->next_seq);
    err = append_locked(ctx, ctx->write_scratch, len);
    if (err != ESP_OK) {
        return err;
    }
    e->state = (uint8_t)new_state;
    ctx->updates_since_compaction++;
    return ESP_OK;
}

static esp_err_t scan_region(edgesync_flashq_ctx_t *ctx, int region, uint32_t generation)
{
    ctx->active_region = region;
    ctx->active_generation = generation;
    ctx->index_count = 0;
    ctx->awaiting_gc_count = 0;
    uint32_t max_seq_seen = 0;
    size_t off = ctx->data_start;

    while (off + EDGESYNC_FLASHQ_HDR_SIZE <= ctx->region_size) {
        uint8_t hdr_buf[EDGESYNC_FLASHQ_HDR_SIZE];
        esp_err_t err = esp_partition_read(ctx->partition, region_offset(ctx, region) + off,
                                            hdr_buf, sizeof(hdr_buf));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "recovery: read failed at offset %u, stopping scan", (unsigned)off);
            break;
        }

        edgesync_flashq_record_hdr_t hdr;
        if (!edgesync_flashq_decode_header(hdr_buf, sizeof(hdr_buf), &hdr)) {
            break; /* end of valid log: blank flash or a torn/corrupt tail record */
        }

        size_t body_bytes = (size_t)hdr.destination_len + hdr.body_len;
        size_t total_needed = EDGESYNC_FLASHQ_HDR_SIZE + body_bytes + sizeof(uint32_t);
        if (off + total_needed > ctx->region_size) {
            ESP_LOGW(TAG, "recovery: record at %u overruns region bounds, stopping scan", (unsigned)off);
            break;
        }

        err = esp_partition_read(ctx->partition, region_offset(ctx, region) + off + EDGESYNC_FLASHQ_HDR_SIZE,
                                  ctx->verify_scratch, body_bytes + sizeof(uint32_t));
        if (err != ESP_OK || !edgesync_flashq_verify_body(&hdr, ctx->verify_scratch, body_bytes + sizeof(uint32_t))) {
            ESP_LOGW(TAG, "recovery: torn/corrupt record at offset %u, stopping scan", (unsigned)off);
            break;
        }

        if (hdr.seq > max_seq_seen) {
            max_seq_seen = hdr.seq;
        }

        if (hdr.type == EDGESYNC_FLASHQ_REC_MESSAGE) {
            if (ctx->index_count >= ctx->index_capacity) {
                ESP_LOGE(TAG, "recovery: index capacity exceeded, dropping message %016llx from index"
                              " (reduce queue growth or increase max_queue_size before next reboot)",
                         (unsigned long long)hdr.message_id);
            } else {
                flashq_index_entry_t *e = &ctx->index[ctx->index_count++];
                e->id = hdr.message_id;
                e->state = hdr.state;
                e->priority = hdr.priority;
                e->destination_len = hdr.destination_len;
                e->attempt_count = hdr.attempt_count;
                e->payload_len = hdr.body_len;
                e->message_offset = (uint32_t)off;
                e->created_at_us = hdr.created_at_us;
                e->last_attempt_at_us = hdr.last_attempt_at_us;
                e->next_retry_at_us = hdr.next_retry_at_us;
                e->last_error = hdr.last_error;
            }
        } else if (hdr.type == EDGESYNC_FLASHQ_REC_UPDATE) {
            flashq_index_entry_t *e = find_entry(ctx, hdr.message_id);
            if (e == NULL) {
                ESP_LOGW(TAG, "recovery: UPDATE record for unknown id %016llx, ignoring",
                         (unsigned long long)hdr.message_id);
            } else {
                e->state = hdr.state;
                e->attempt_count = hdr.attempt_count;
                e->last_attempt_at_us = hdr.last_attempt_at_us;
                e->next_retry_at_us = hdr.next_retry_at_us;
                e->last_error = hdr.last_error;
                if (hdr.state == EDGESYNC_FLASHQ_STATE_DELIVERED || hdr.state == EDGESYNC_FLASHQ_STATE_REMOVED) {
                    size_t pos = (size_t)(e - ctx->index);
                    remove_entry_at(ctx, pos);
                    ctx->awaiting_gc_count++;
                }
            }
        } /* EDGESYNC_FLASHQ_REC_END: no-op marker */

        off += edgesync_flashq_round4(total_needed);
    }

    ctx->cursor = off;
    ctx->next_seq = max_seq_seen + 1;

    /* An IN_FLIGHT message means the worker was mid-delivery when the
     * device stopped (crash/reset/power loss). We cannot know whether the
     * destination received it, so per the at-least-once contract we make it
     * eligible for delivery again rather than silently dropping it. */
    for (size_t i = 0; i < ctx->index_count; i++) {
        if (ctx->index[i].state == EDGESYNC_FLASHQ_STATE_IN_FLIGHT) {
            ESP_LOGW(TAG, "recovery: message %016llx was IN_FLIGHT at restart, requeuing as PENDING",
                     (unsigned long long)ctx->index[i].id);
            ctx->index[i].next_retry_at_us = 0;
            esp_err_t err = append_state_update_locked(ctx, &ctx->index[i], EDGESYNC_FLASHQ_STATE_PENDING);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * edgesync_storage_ops_t implementation
 * ---------------------------------------------------------------------- */

static esp_err_t op_init(void *vctx)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    ctx->lock = xSemaphoreCreateMutex();
    if (ctx->lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t op_recover(void *vctx)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    edgesync_flashq_region_hdr_t hdrs[EDGESYNC_FLASHQ_NUM_REGIONS];
    bool blank[EDGESYNC_FLASHQ_NUM_REGIONS];
    bool valid[EDGESYNC_FLASHQ_NUM_REGIONS];

    for (int i = 0; i < EDGESYNC_FLASHQ_NUM_REGIONS; i++) {
        esp_err_t err = region_header_read(ctx, i, &hdrs[i], &blank[i]);
        valid[i] = (err == ESP_OK) && !blank[i] && (hdrs[i].magic == EDGESYNC_FLASHQ_HDR_MAGIC) &&
                   (hdrs[i].status == EDGESYNC_FLASHQ_STATUS_ACTIVE ||
                    hdrs[i].status == EDGESYNC_FLASHQ_STATUS_COMPACTING ||
                    hdrs[i].status == EDGESYNC_FLASHQ_STATUS_STALE);
    }

    bool active0 = valid[0] && hdrs[0].status == EDGESYNC_FLASHQ_STATUS_ACTIVE;
    bool active1 = valid[1] && hdrs[1].status == EDGESYNC_FLASHQ_STATUS_ACTIVE;

    int chosen = -1;
    if (active0 && active1) {
        ESP_LOGW(TAG, "both regions marked ACTIVE (interrupted compaction finalize); using higher generation");
        chosen = (hdrs[0].generation >= hdrs[1].generation) ? 0 : 1;
    } else if (active0) {
        chosen = 0;
    } else if (active1) {
        chosen = 1;
    }

    esp_err_t err;
    if (chosen < 0) {
        if (!valid[0] && !valid[1]) {
            ESP_LOGI(TAG, "flash queue partition is empty; initializing region 0");
        } else {
            ESP_LOGE(TAG, "flash queue partition has no valid ACTIVE region; reinitializing (data lost)");
        }
        err = erase_region(ctx, 0);
        if (err == ESP_OK) {
            err = erase_region(ctx, 1);
        }
        if (err != ESP_OK) {
            xSemaphoreGive(ctx->lock);
            return err;
        }
        edgesync_flashq_region_hdr_t fresh = {
            .magic = EDGESYNC_FLASHQ_HDR_MAGIC, .generation = 1, .status = EDGESYNC_FLASHQ_STATUS_ACTIVE, .reserved = {0},
        };
        err = partition_write_verify(ctx, region_offset(ctx, 0), &fresh, sizeof(fresh));
        if (err != ESP_OK) {
            xSemaphoreGive(ctx->lock);
            return err;
        }
        ctx->index_count = 0;
        ctx->cursor = ctx->data_start;
        ctx->next_seq = 1;
        ctx->active_region = 0;
        ctx->active_generation = 1;
        ctx->awaiting_gc_count = 0;
        ctx->compactions_total = 0;
        xSemaphoreGive(ctx->lock);
        return ESP_OK;
    }

    err = ensure_region_erased(ctx, 1 - chosen);
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->lock);
        return err;
    }

    err = scan_region(ctx, chosen, hdrs[chosen].generation);
    xSemaphoreGive(ctx->lock);
    return err;
}

static esp_err_t op_enqueue(void *vctx, const edgesync_message_t *message)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    if (ctx->index_count >= ctx->index_capacity) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_QUEUE_FULL;
    }

    size_t enc_len = 0;
    if (!edgesync_flashq_encode_message(ctx->write_scratch, ctx->scratch_size, &enc_len, message, ctx->next_seq)) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_SERIALIZATION;
    }

    esp_err_t err = ensure_space_locked(ctx, enc_len);
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->lock);
        return err;
    }
    /* ensure_space_locked() may have compacted, which reassigns ctx->next_seq;
     * re-encode with the current sequence number to keep it monotonic. */
    if (!edgesync_flashq_encode_message(ctx->write_scratch, ctx->scratch_size, &enc_len, message, ctx->next_seq)) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_SERIALIZATION;
    }

    size_t offset = ctx->cursor;
    err = append_locked(ctx, ctx->write_scratch, enc_len);
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->lock);
        return err;
    }

    flashq_index_entry_t *e = &ctx->index[ctx->index_count++];
    e->id = message->id;
    e->state = EDGESYNC_FLASHQ_STATE_PENDING;
    e->priority = (uint8_t)message->priority;
    e->destination_len = (uint16_t)strlen(message->destination);
    e->attempt_count = 0;
    e->payload_len = message->payload_len;
    e->message_offset = (uint32_t)offset;
    e->created_at_us = message->created_at_us;
    e->last_attempt_at_us = 0;
    e->next_retry_at_us = 0;
    e->last_error = ESP_OK;

    xSemaphoreGive(ctx->lock);
    return ESP_OK;
}

static esp_err_t op_claim_next(void *vctx, int64_t now_us, edgesync_message_t *out_message)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    flashq_index_entry_t *best = NULL;
    for (size_t i = 0; i < ctx->index_count; i++) {
        flashq_index_entry_t *e = &ctx->index[i];
        if (e->state != EDGESYNC_FLASHQ_STATE_PENDING || e->next_retry_at_us > now_us) {
            continue;
        }
        if (best == NULL || e->priority > best->priority ||
            (e->priority == best->priority && e->next_retry_at_us < best->next_retry_at_us) ||
            (e->priority == best->priority && e->next_retry_at_us == best->next_retry_at_us &&
             e->created_at_us < best->created_at_us)) {
            best = e;
        }
    }

    if (best == NULL) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_NOT_FOUND;
    }

    size_t body_bytes = (size_t)best->destination_len + best->payload_len;
    size_t read_len = EDGESYNC_FLASHQ_HDR_SIZE + body_bytes + sizeof(uint32_t);
    size_t abs_off = region_offset(ctx, ctx->active_region) + best->message_offset;

    esp_err_t err = esp_partition_read(ctx->partition, abs_off, ctx->claim_scratch, read_len);
    edgesync_flashq_record_hdr_t hdr;
    bool corrupt = (err != ESP_OK) || !edgesync_flashq_decode_header(ctx->claim_scratch, read_len, &hdr) ||
                   !edgesync_flashq_verify_body(&hdr, ctx->claim_scratch + EDGESYNC_FLASHQ_HDR_SIZE,
                                                 read_len - EDGESYNC_FLASHQ_HDR_SIZE);

    if (corrupt) {
        ESP_LOGE(TAG, "flash corruption detected for message %016llx at delivery time; dead-lettering",
                 (unsigned long long)best->id);
        best->last_error = EDGESYNC_ERR_STORAGE_CORRUPT;
        err = append_state_update_locked(ctx, best, EDGESYNC_FLASHQ_STATE_DEAD_LETTER);
        memset(out_message, 0, sizeof(*out_message));
        out_message->id = best->id;
        xSemaphoreGive(ctx->lock);
        return (err == ESP_OK) ? EDGESYNC_ERR_STORAGE_CORRUPT : err;
    }

    uint32_t new_attempt_count = best->attempt_count + 1;
    size_t needed = edgesync_flashq_record_total_size(0, 0);
    err = ensure_space_locked(ctx, needed);
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->lock);
        return err;
    }

    size_t upd_len = 0;
    edgesync_flashq_encode_update(ctx->write_scratch, ctx->scratch_size, &upd_len, best->id,
                                   EDGESYNC_FLASHQ_STATE_IN_FLIGHT, new_attempt_count, now_us,
                                   best->next_retry_at_us, best->last_error, ctx->next_seq);
    err = append_locked(ctx, ctx->write_scratch, upd_len);
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->lock);
        return err;
    }

    best->state = EDGESYNC_FLASHQ_STATE_IN_FLIGHT;
    best->attempt_count = new_attempt_count;
    best->last_attempt_at_us = now_us;
    ctx->updates_since_compaction++;

    memset(out_message, 0, sizeof(*out_message));
    out_message->id = best->id;
    memcpy(out_message->destination, ctx->claim_scratch + EDGESYNC_FLASHQ_HDR_SIZE, best->destination_len);
    out_message->destination[best->destination_len] = '\0';
    out_message->priority = (edgesync_priority_t)best->priority;
    out_message->state = EDGESYNC_MSG_IN_FLIGHT;
    out_message->attempt_count = new_attempt_count;
    out_message->created_at_us = best->created_at_us;
    out_message->last_attempt_at_us = now_us;
    out_message->next_retry_at_us = best->next_retry_at_us;
    out_message->last_error = best->last_error;
    out_message->payload_len = best->payload_len;
    out_message->payload = ctx->claim_scratch + EDGESYNC_FLASHQ_HDR_SIZE + best->destination_len;

    xSemaphoreGive(ctx->lock);
    return ESP_OK;
}

static esp_err_t op_mark_delivered(void *vctx, edgesync_message_id_t id)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    flashq_index_entry_t *e = find_entry(ctx, id);
    if (e == NULL) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_NOT_FOUND;
    }

    esp_err_t err = append_state_update_locked(ctx, e, EDGESYNC_FLASHQ_STATE_DELIVERED);
    if (err == ESP_OK) {
        size_t pos = (size_t)(e - ctx->index);
        remove_entry_at(ctx, pos);
        ctx->awaiting_gc_count++;
    }

    xSemaphoreGive(ctx->lock);
    return err;
}

static esp_err_t op_schedule_retry(void *vctx, edgesync_message_id_t id, int64_t next_retry_time_us,
                                    bool dead_letter, esp_err_t last_error)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    flashq_index_entry_t *e = find_entry(ctx, id);
    if (e == NULL) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_NOT_FOUND;
    }

    e->next_retry_at_us = dead_letter ? 0 : next_retry_time_us;
    e->last_error = last_error;
    esp_err_t err = append_state_update_locked(ctx, e,
        dead_letter ? EDGESYNC_FLASHQ_STATE_DEAD_LETTER : EDGESYNC_FLASHQ_STATE_PENDING);

    xSemaphoreGive(ctx->lock);
    return err;
}

static esp_err_t op_remove(void *vctx, edgesync_message_id_t id)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    flashq_index_entry_t *e = find_entry(ctx, id);
    if (e == NULL) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_NOT_FOUND;
    }

    esp_err_t err = append_state_update_locked(ctx, e, EDGESYNC_FLASHQ_STATE_REMOVED);
    if (err == ESP_OK) {
        size_t pos = (size_t)(e - ctx->index);
        remove_entry_at(ctx, pos);
        ctx->awaiting_gc_count++;
    }

    xSemaphoreGive(ctx->lock);
    return err;
}

static esp_err_t op_find_drop_candidate(void *vctx, edgesync_overflow_policy_t policy, edgesync_message_id_t *out_id)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    flashq_index_entry_t *best = NULL;
    for (size_t i = 0; i < ctx->index_count; i++) {
        flashq_index_entry_t *e = &ctx->index[i];
        if (e->state != EDGESYNC_FLASHQ_STATE_PENDING) {
            continue;
        }
        if (best == NULL) {
            best = e;
            continue;
        }
        if (policy == EDGESYNC_OVERFLOW_DROP_LOWEST_PRIORITY) {
            if (e->priority < best->priority ||
                (e->priority == best->priority && e->created_at_us < best->created_at_us)) {
                best = e;
            }
        } else { /* DROP_OLDEST (and fallback default) */
            if (e->created_at_us < best->created_at_us) {
                best = e;
            }
        }
    }

    esp_err_t result = ESP_OK;
    if (best == NULL) {
        result = EDGESYNC_ERR_NOT_FOUND;
    } else {
        *out_id = best->id;
    }

    xSemaphoreGive(ctx->lock);
    return result;
}

static esp_err_t op_get_stats(void *vctx, edgesync_stats_t *stats)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    for (size_t i = 0; i < ctx->index_count; i++) {
        flashq_index_entry_t *e = &ctx->index[i];
        switch ((edgesync_flashq_state_t)e->state) {
        case EDGESYNC_FLASHQ_STATE_PENDING:
            if (e->attempt_count == 0) {
                stats->pending_messages++;
            } else {
                stats->retrying_messages++;
            }
            stats->bytes_queued += e->payload_len;
            break;
        case EDGESYNC_FLASHQ_STATE_IN_FLIGHT:
            stats->in_flight_messages++;
            stats->bytes_queued += e->payload_len;
            break;
        case EDGESYNC_FLASHQ_STATE_DEAD_LETTER:
            stats->dead_letter_messages++;
            stats->bytes_queued += e->payload_len;
            break;
        default:
            break;
        }
    }
    stats->delivered_awaiting_gc = ctx->awaiting_gc_count;
    stats->queue_capacity = (uint32_t)ctx->index_capacity;
    stats->storage_bytes_used = (uint32_t)(ctx->cursor - ctx->data_start);
    stats->storage_bytes_capacity = (uint32_t)ctx->data_capacity;
    stats->compactions_total = ctx->compactions_total;

    xSemaphoreGive(ctx->lock);
    return ESP_OK;
}

static esp_err_t op_close(void *vctx)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    if (ctx->lock) {
        vSemaphoreDelete(ctx->lock);
        ctx->lock = NULL;
    }
    return ESP_OK;
}

static const edgesync_storage_ops_t s_flashq_ops = {
    .init = op_init,
    .recover = op_recover,
    .enqueue = op_enqueue,
    .claim_next = op_claim_next,
    .mark_delivered = op_mark_delivered,
    .schedule_retry = op_schedule_retry,
    .remove = op_remove,
    .find_drop_candidate = op_find_drop_candidate,
    .get_stats = op_get_stats,
    .close = op_close,
};

/* ------------------------------------------------------------------------
 * Constructor / destructor
 * ---------------------------------------------------------------------- */

esp_err_t edgesync_flashq_create(const edgesync_flashq_config_t *config,
                                  const edgesync_storage_ops_t **out_ops, void **out_ctx)
{
    if (config == NULL || out_ops == NULL || out_ctx == NULL || config->max_messages == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *label = config->partition_label ? config->partition_label : "edgesync";
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                  ESP_PARTITION_SUBTYPE_ANY, label);
    if (partition == NULL) {
        ESP_LOGE(TAG, "flash queue partition \"%s\" not found - add it to your partitions.csv"
                      " (see STORAGE.md)", label);
        return EDGESYNC_ERR_STORAGE;
    }

    size_t total_sectors = partition->size / EDGESYNC_FLASHQ_SECTOR_SIZE;
    if (partition->size % EDGESYNC_FLASHQ_SECTOR_SIZE != 0 || total_sectors < 4) {
        ESP_LOGE(TAG, "flash queue partition \"%s\" (%u bytes) is too small or not sector-aligned;"
                      " need >= %u bytes", label, (unsigned)partition->size, 4 * EDGESYNC_FLASHQ_SECTOR_SIZE);
        return EDGESYNC_ERR_STORAGE;
    }

    edgesync_flashq_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ctx->partition = partition;
    ctx->sectors_per_region = total_sectors / EDGESYNC_FLASHQ_NUM_REGIONS;
    ctx->region_size = ctx->sectors_per_region * EDGESYNC_FLASHQ_SECTOR_SIZE;
    ctx->data_start = EDGESYNC_FLASHQ_SECTOR_SIZE;
    ctx->data_capacity = ctx->region_size - ctx->data_start;
    ctx->max_message_size = config->max_message_size;
    ctx->index_capacity = config->max_messages;

    ctx->index = calloc(ctx->index_capacity, sizeof(flashq_index_entry_t));
    ctx->scratch_size = max_record_size(ctx);
    ctx->write_scratch = malloc(ctx->scratch_size);
    ctx->verify_scratch = malloc(ctx->scratch_size);
    ctx->claim_scratch = malloc(ctx->scratch_size);

    if (!ctx->index || !ctx->write_scratch || !ctx->verify_scratch || !ctx->claim_scratch) {
        edgesync_flashq_destroy(ctx);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "flash queue: partition \"%s\" %u bytes, %u regions x %u sectors, max %u messages,"
                  " max %u bytes/message",
             label, (unsigned)partition->size, EDGESYNC_FLASHQ_NUM_REGIONS, (unsigned)ctx->sectors_per_region,
             (unsigned)ctx->index_capacity, (unsigned)ctx->max_message_size);

    *out_ops = &s_flashq_ops;
    *out_ctx = ctx;
    return ESP_OK;
}

void edgesync_flashq_destroy(void *vctx)
{
    edgesync_flashq_ctx_t *ctx = vctx;
    if (ctx == NULL) {
        return;
    }
    free(ctx->index);
    free(ctx->write_scratch);
    free(ctx->verify_scratch);
    free(ctx->claim_scratch);
    free(ctx);
}
