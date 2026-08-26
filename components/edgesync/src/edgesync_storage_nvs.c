/**
 * @file edgesync_storage_nvs.c
 * @brief NVS-backed storage backend - simple, but only for small/low-frequency queues.
 *
 * Each message is stored as a single NVS blob keyed by a slot number
 * ("s00000".."sNNNNN"). The blob is the same self-describing,
 * CRC-protected record format used by the flash-queue backend
 * (edgesync_flashq_format.h), reused here purely as a validated
 * serialization format - there is no log/compaction machinery.
 *
 * Because NVS blobs are rewritten wholesale, every state transition
 * (claim -> IN_FLIGHT, retry, dead-letter) rewrites the full record
 * INCLUDING the payload, and every durable write costs an nvs_commit().
 * This is fine for a handful of infrequent messages; it is not
 * appropriate for telemetry-rate publishing - see STORAGE.md.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "edgesync/edgesync.h"
#include "edgesync_storage_nvs.h"
#include "edgesync_flashq_format.h"

static const char *TAG = "EdgeSync";

typedef struct {
    edgesync_message_id_t id;
    uint8_t state;
    uint8_t priority;
    uint16_t destination_len;
    uint32_t attempt_count;
    uint32_t payload_len;
    uint32_t slot;
    int64_t created_at_us;
    int64_t last_attempt_at_us;
    int64_t next_retry_at_us;
    int32_t last_error;
} nvs_index_entry_t;

typedef struct {
    nvs_handle_t handle;
    uint32_t max_messages;
    uint32_t max_message_size;

    bool *slot_used;
    nvs_index_entry_t *index;
    size_t index_count;
    size_t index_capacity;

    size_t scratch_size;
    uint8_t *scratch;       /* encode/decode scratch, reused under lock */
    uint8_t *claim_scratch; /* dedicated buffer backing claim_next()'s borrowed payload pointer */

    SemaphoreHandle_t lock;
} edgesync_nvs_ctx_t;

static void slot_key(uint32_t slot, char out[16])
{
    snprintf(out, 16, "s%05u", (unsigned)(slot % 100000u));
}

static nvs_index_entry_t *find_entry(edgesync_nvs_ctx_t *ctx, edgesync_message_id_t id)
{
    for (size_t i = 0; i < ctx->index_count; i++) {
        if (ctx->index[i].id == id) {
            return &ctx->index[i];
        }
    }
    return NULL;
}

static void remove_entry_at(edgesync_nvs_ctx_t *ctx, size_t pos)
{
    ctx->slot_used[ctx->index[pos].slot] = false;
    ctx->index[pos] = ctx->index[ctx->index_count - 1];
    ctx->index_count--;
}

static bool find_free_slot(edgesync_nvs_ctx_t *ctx, uint32_t *out_slot)
{
    for (uint32_t i = 0; i < ctx->max_messages; i++) {
        if (!ctx->slot_used[i]) {
            *out_slot = i;
            return true;
        }
    }
    return false;
}

/** Write the full current state of `e` (metadata from `e`, body from `body`/`body_len`) to its slot. */
static esp_err_t write_entry(edgesync_nvs_ctx_t *ctx, nvs_index_entry_t *e, const char *destination,
                              const void *payload)
{
    size_t enc_len = 0;
    if (!edgesync_flashq_encode_message_full(ctx->scratch, ctx->scratch_size, &enc_len, e->id, destination,
                                              (edgesync_priority_t)e->priority, (edgesync_flashq_state_t)e->state,
                                              e->attempt_count, e->created_at_us, e->last_attempt_at_us,
                                              e->next_retry_at_us, e->last_error, payload, e->payload_len, 0)) {
        return EDGESYNC_ERR_SERIALIZATION;
    }

    char key[16];
    slot_key(e->slot, key);
    esp_err_t err = nvs_set_blob(ctx->handle, key, ctx->scratch, enc_len);
    if (err != ESP_OK) {
        return EDGESYNC_ERR_STORAGE;
    }
    err = nvs_commit(ctx->handle);
    return (err == ESP_OK) ? ESP_OK : EDGESYNC_ERR_STORAGE;
}

/** Read and validate the blob at `slot` into ctx->scratch. On success, *out_hdr is populated. */
static esp_err_t read_and_verify(edgesync_nvs_ctx_t *ctx, uint32_t slot, edgesync_flashq_record_hdr_t *out_hdr)
{
    char key[16];
    slot_key(slot, key);

    size_t len = 0;
    esp_err_t err = nvs_get_blob(ctx->handle, key, NULL, &len);
    if (err != ESP_OK) {
        return EDGESYNC_ERR_NOT_FOUND;
    }
    if (len > ctx->scratch_size) {
        ESP_LOGE(TAG, "nvs slot %u blob larger than expected (%u > %u)", (unsigned)slot,
                 (unsigned)len, (unsigned)ctx->scratch_size);
        return EDGESYNC_ERR_STORAGE_CORRUPT;
    }
    err = nvs_get_blob(ctx->handle, key, ctx->scratch, &len);
    if (err != ESP_OK) {
        return EDGESYNC_ERR_STORAGE;
    }
    if (!edgesync_flashq_decode_header(ctx->scratch, len, out_hdr) ||
        !edgesync_flashq_verify_body(out_hdr, ctx->scratch + EDGESYNC_FLASHQ_HDR_SIZE,
                                      len - EDGESYNC_FLASHQ_HDR_SIZE)) {
        return EDGESYNC_ERR_STORAGE_CORRUPT;
    }
    return ESP_OK;
}

static esp_err_t op_init(void *vctx)
{
    edgesync_nvs_ctx_t *ctx = vctx;
    ctx->lock = xSemaphoreCreateMutex();
    return ctx->lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t op_recover(void *vctx)
{
    edgesync_nvs_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    ctx->index_count = 0;
    memset(ctx->slot_used, 0, ctx->max_messages * sizeof(bool));

    for (uint32_t slot = 0; slot < ctx->max_messages; slot++) {
        edgesync_flashq_record_hdr_t hdr;
        esp_err_t err = read_and_verify(ctx, slot, &hdr);
        if (err == EDGESYNC_ERR_NOT_FOUND) {
            continue;
        }
        if (err == EDGESYNC_ERR_STORAGE_CORRUPT) {
            ESP_LOGE(TAG, "nvs slot %u corrupt, discarding (self-healing)", (unsigned)slot);
            char key[16];
            slot_key(slot, key);
            nvs_erase_key(ctx->handle, key);
            nvs_commit(ctx->handle);
            continue;
        }
        if (err != ESP_OK) {
            xSemaphoreGive(ctx->lock);
            return err;
        }

        if (ctx->index_count >= ctx->index_capacity) {
            ESP_LOGE(TAG, "nvs recovery: index capacity exceeded, dropping slot %u", (unsigned)slot);
            continue;
        }
        nvs_index_entry_t *e = &ctx->index[ctx->index_count++];
        e->id = hdr.message_id;
        e->state = hdr.state;
        e->priority = hdr.priority;
        e->destination_len = hdr.destination_len;
        e->attempt_count = hdr.attempt_count;
        e->payload_len = hdr.body_len;
        e->slot = slot;
        e->created_at_us = hdr.created_at_us;
        e->last_attempt_at_us = hdr.last_attempt_at_us;
        e->next_retry_at_us = hdr.next_retry_at_us;
        e->last_error = hdr.last_error;
        ctx->slot_used[slot] = true;
    }

    for (size_t i = 0; i < ctx->index_count; i++) {
        nvs_index_entry_t *e = &ctx->index[i];
        if (e->state != EDGESYNC_FLASHQ_STATE_IN_FLIGHT) {
            continue;
        }
        ESP_LOGW(TAG, "recovery: message %016llx was IN_FLIGHT at restart, requeuing as PENDING",
                 (unsigned long long)e->id);
        edgesync_flashq_record_hdr_t hdr;
        esp_err_t err = read_and_verify(ctx, e->slot, &hdr);
        if (err != ESP_OK) {
            xSemaphoreGive(ctx->lock);
            return err;
        }
        char destination[EDGESYNC_MAX_DESTINATION_LEN + 1] = {0};
        memcpy(destination, ctx->scratch + EDGESYNC_FLASHQ_HDR_SIZE, hdr.destination_len);
        const uint8_t *payload = ctx->scratch + EDGESYNC_FLASHQ_HDR_SIZE + hdr.destination_len;
        e->state = EDGESYNC_FLASHQ_STATE_PENDING;
        e->next_retry_at_us = 0;
        err = write_entry(ctx, e, destination, payload);
        if (err != ESP_OK) {
            xSemaphoreGive(ctx->lock);
            return err;
        }
    }

    xSemaphoreGive(ctx->lock);
    return ESP_OK;
}

static esp_err_t op_enqueue(void *vctx, const edgesync_message_t *message)
{
    edgesync_nvs_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    uint32_t slot;
    if (ctx->index_count >= ctx->index_capacity || !find_free_slot(ctx, &slot)) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_QUEUE_FULL;
    }

    nvs_index_entry_t e = {
        .id = message->id,
        .state = EDGESYNC_FLASHQ_STATE_PENDING,
        .priority = (uint8_t)message->priority,
        .destination_len = (uint16_t)strlen(message->destination),
        .attempt_count = 0,
        .payload_len = message->payload_len,
        .slot = slot,
        .created_at_us = message->created_at_us,
        .last_attempt_at_us = 0,
        .next_retry_at_us = 0,
        .last_error = ESP_OK,
    };

    esp_err_t err = write_entry(ctx, &e, message->destination, message->payload);
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->lock);
        return err;
    }

    ctx->index[ctx->index_count++] = e;
    ctx->slot_used[slot] = true;

    xSemaphoreGive(ctx->lock);
    return ESP_OK;
}

static esp_err_t op_claim_next(void *vctx, int64_t now_us, edgesync_message_t *out_message)
{
    edgesync_nvs_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    nvs_index_entry_t *best = NULL;
    for (size_t i = 0; i < ctx->index_count; i++) {
        nvs_index_entry_t *e = &ctx->index[i];
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

    edgesync_flashq_record_hdr_t hdr;
    esp_err_t err = read_and_verify(ctx, best->slot, &hdr);
    if (err == EDGESYNC_ERR_STORAGE_CORRUPT) {
        ESP_LOGE(TAG, "nvs corruption detected for message %016llx at delivery time; dead-lettering",
                 (unsigned long long)best->id);
        best->last_error = EDGESYNC_ERR_STORAGE_CORRUPT;
        best->state = EDGESYNC_FLASHQ_STATE_DEAD_LETTER;
        /* Cannot recover the payload to rewrite a full record; erase the slot so it
         * stops occupying space, keep the index entry so the dead-letter is still visible. */
        char key[16];
        slot_key(best->slot, key);
        nvs_erase_key(ctx->handle, key);
        nvs_commit(ctx->handle);
        memset(out_message, 0, sizeof(*out_message));
        out_message->id = best->id;
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_STORAGE_CORRUPT;
    }
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->lock);
        return err;
    }

    memcpy(ctx->claim_scratch, ctx->scratch + EDGESYNC_FLASHQ_HDR_SIZE, (size_t)hdr.destination_len + hdr.body_len);

    best->attempt_count += 1;
    best->state = EDGESYNC_FLASHQ_STATE_IN_FLIGHT;
    best->last_attempt_at_us = now_us;

    char destination[EDGESYNC_MAX_DESTINATION_LEN + 1] = {0};
    memcpy(destination, ctx->claim_scratch, best->destination_len);
    err = write_entry(ctx, best, destination, ctx->claim_scratch + best->destination_len);
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->lock);
        return err;
    }

    memset(out_message, 0, sizeof(*out_message));
    out_message->id = best->id;
    memcpy(out_message->destination, destination, best->destination_len);
    out_message->priority = (edgesync_priority_t)best->priority;
    out_message->state = EDGESYNC_MSG_IN_FLIGHT;
    out_message->attempt_count = best->attempt_count;
    out_message->created_at_us = best->created_at_us;
    out_message->last_attempt_at_us = now_us;
    out_message->next_retry_at_us = best->next_retry_at_us;
    out_message->last_error = best->last_error;
    out_message->payload_len = best->payload_len;
    out_message->payload = ctx->claim_scratch + best->destination_len;

    xSemaphoreGive(ctx->lock);
    return ESP_OK;
}

static esp_err_t op_mark_delivered(void *vctx, edgesync_message_id_t id)
{
    edgesync_nvs_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    nvs_index_entry_t *e = find_entry(ctx, id);
    if (e == NULL) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_NOT_FOUND;
    }

    char key[16];
    slot_key(e->slot, key);
    esp_err_t err = nvs_erase_key(ctx->handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(ctx->handle);
    }
    if (err == ESP_OK) {
        size_t pos = (size_t)(e - ctx->index);
        remove_entry_at(ctx, pos);
    } else {
        err = EDGESYNC_ERR_STORAGE;
    }

    xSemaphoreGive(ctx->lock);
    return err;
}

static esp_err_t op_schedule_retry(void *vctx, edgesync_message_id_t id, int64_t next_retry_time_us,
                                    bool dead_letter, esp_err_t last_error)
{
    edgesync_nvs_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    nvs_index_entry_t *e = find_entry(ctx, id);
    if (e == NULL) {
        xSemaphoreGive(ctx->lock);
        return EDGESYNC_ERR_NOT_FOUND;
    }

    edgesync_flashq_record_hdr_t hdr;
    esp_err_t err = read_and_verify(ctx, e->slot, &hdr);
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->lock);
        return err;
    }
    char destination[EDGESYNC_MAX_DESTINATION_LEN + 1] = {0};
    memcpy(destination, ctx->scratch + EDGESYNC_FLASHQ_HDR_SIZE, hdr.destination_len);
    const uint8_t *payload = ctx->scratch + EDGESYNC_FLASHQ_HDR_SIZE + hdr.destination_len;

    e->state = dead_letter ? EDGESYNC_FLASHQ_STATE_DEAD_LETTER : EDGESYNC_FLASHQ_STATE_PENDING;
    e->next_retry_at_us = dead_letter ? 0 : next_retry_time_us;
    e->last_error = last_error;

    err = write_entry(ctx, e, destination, payload);

    xSemaphoreGive(ctx->lock);
    return err;
}

static esp_err_t op_remove(void *vctx, edgesync_message_id_t id)
{
    return op_mark_delivered(vctx, id); /* Same effect for NVS: erase the slot. */
}

static esp_err_t op_find_drop_candidate(void *vctx, edgesync_overflow_policy_t policy, edgesync_message_id_t *out_id)
{
    edgesync_nvs_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    nvs_index_entry_t *best = NULL;
    for (size_t i = 0; i < ctx->index_count; i++) {
        nvs_index_entry_t *e = &ctx->index[i];
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
        } else if (e->created_at_us < best->created_at_us) {
            best = e;
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
    edgesync_nvs_ctx_t *ctx = vctx;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);

    for (size_t i = 0; i < ctx->index_count; i++) {
        nvs_index_entry_t *e = &ctx->index[i];
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
    stats->queue_capacity = (uint32_t)ctx->index_capacity;

    nvs_stats_t nvs_stats;
    if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
        /* Whole default NVS partition, not exclusive to EdgeSync - see STORAGE.md. */
        stats->storage_bytes_capacity = (uint32_t)nvs_stats.total_entries * 32u;
        stats->storage_bytes_used = (uint32_t)nvs_stats.used_entries * 32u;
    }

    xSemaphoreGive(ctx->lock);
    return ESP_OK;
}

static esp_err_t op_close(void *vctx)
{
    edgesync_nvs_ctx_t *ctx = vctx;
    nvs_close(ctx->handle);
    if (ctx->lock) {
        vSemaphoreDelete(ctx->lock);
        ctx->lock = NULL;
    }
    return ESP_OK;
}

static const edgesync_storage_ops_t s_nvs_ops = {
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

esp_err_t edgesync_nvs_storage_create(const edgesync_nvs_storage_config_t *config,
                                       const edgesync_storage_ops_t **out_ops, void **out_ctx)
{
    if (config == NULL || out_ops == NULL || out_ctx == NULL || config->max_messages == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    edgesync_nvs_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const char *ns = config->nvs_namespace ? config->nvs_namespace : "edgesync";
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &ctx->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(\"%s\") failed: %d (did you call nvs_flash_init()?)", ns, err);
        free(ctx);
        return EDGESYNC_ERR_STORAGE;
    }

    ctx->max_messages = config->max_messages;
    ctx->max_message_size = config->max_message_size;
    ctx->index_capacity = config->max_messages;
    ctx->scratch_size = edgesync_flashq_record_total_size((uint16_t)EDGESYNC_MAX_DESTINATION_LEN,
                                                            config->max_message_size);

    ctx->slot_used = calloc(ctx->max_messages, sizeof(bool));
    ctx->index = calloc(ctx->index_capacity, sizeof(nvs_index_entry_t));
    ctx->scratch = malloc(ctx->scratch_size);
    ctx->claim_scratch = malloc(ctx->scratch_size);

    if (!ctx->slot_used || !ctx->index || !ctx->scratch || !ctx->claim_scratch) {
        edgesync_nvs_storage_destroy(ctx);
        return ESP_ERR_NO_MEM;
    }

    if (ctx->max_messages > 200) {
        ESP_LOGW(TAG, "NVS storage backend configured for %u messages - this backend is only"
                      " recommended for small, low-frequency queues (see STORAGE.md)",
                 (unsigned)ctx->max_messages);
    }

    ESP_LOGI(TAG, "nvs storage: namespace \"%s\", max %u messages, max %u bytes/message",
             ns, (unsigned)ctx->max_messages, (unsigned)ctx->max_message_size);

    *out_ops = &s_nvs_ops;
    *out_ctx = ctx;
    return ESP_OK;
}

void edgesync_nvs_storage_destroy(void *vctx)
{
    edgesync_nvs_ctx_t *ctx = vctx;
    if (ctx == NULL) {
        return;
    }
    free(ctx->slot_used);
    free(ctx->index);
    free(ctx->scratch);
    free(ctx->claim_scratch);
    free(ctx);
}
