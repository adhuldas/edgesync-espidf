#include <string.h>
#include "edgesync_flashq_format.h"
#include "edgesync_crc32.h"

static void put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t header_crc(const edgesync_flashq_record_hdr_t *hdr)
{
    edgesync_flashq_record_hdr_t tmp = *hdr;
    tmp.header_crc = 0;
    return edgesync_crc32(0, &tmp, sizeof(tmp));
}

static bool encode_common(uint8_t *buf, size_t buf_cap, size_t *out_len,
                           edgesync_flashq_record_hdr_t *hdr,
                           const uint8_t *destination, uint16_t destination_len,
                           const uint8_t *body, uint32_t body_len)
{
    size_t total = edgesync_flashq_record_total_size(destination_len, body_len);
    if (buf_cap < total) {
        return false;
    }

    hdr->magic = EDGESYNC_FLASHQ_REC_MAGIC;
    hdr->destination_len = destination_len;
    hdr->body_len = body_len;
    hdr->header_crc = 0;
    hdr->header_crc = header_crc(hdr);

    memset(buf, 0, total); /* zero padding bytes deterministically */
    memcpy(buf, hdr, EDGESYNC_FLASHQ_HDR_SIZE);

    uint8_t *cursor = buf + EDGESYNC_FLASHQ_HDR_SIZE;
    if (destination_len > 0 && destination != NULL) {
        memcpy(cursor, destination, destination_len);
    }
    cursor += destination_len;
    if (body_len > 0 && body != NULL) {
        memcpy(cursor, body, body_len);
    }
    cursor += body_len;

    uint32_t crc = edgesync_crc32(0, buf + EDGESYNC_FLASHQ_HDR_SIZE, (size_t)destination_len + body_len);
    put_u32(cursor, crc);

    *out_len = total;
    return true;
}

bool edgesync_flashq_encode_message_full(uint8_t *buf, size_t buf_cap, size_t *out_len,
                                          edgesync_message_id_t id, const char *destination,
                                          edgesync_priority_t priority, edgesync_flashq_state_t state,
                                          uint32_t attempt_count, int64_t created_at_us,
                                          int64_t last_attempt_at_us, int64_t next_retry_at_us,
                                          int32_t last_error, const void *payload, uint32_t payload_len,
                                          uint32_t seq)
{
    size_t dest_len = strlen(destination);
    if (dest_len > EDGESYNC_MAX_DESTINATION_LEN) {
        return false;
    }

    edgesync_flashq_record_hdr_t hdr = {0};
    hdr.seq = seq;
    hdr.type = EDGESYNC_FLASHQ_REC_MESSAGE;
    hdr.state = (uint8_t)state;
    hdr.priority = (uint8_t)priority;
    hdr.message_id = id;
    hdr.attempt_count = attempt_count;
    hdr.created_at_us = created_at_us;
    hdr.last_attempt_at_us = last_attempt_at_us;
    hdr.next_retry_at_us = next_retry_at_us;
    hdr.last_error = last_error;

    return encode_common(buf, buf_cap, out_len, &hdr,
                          (const uint8_t *)destination, (uint16_t)dest_len,
                          (const uint8_t *)payload, payload_len);
}

bool edgesync_flashq_encode_message(uint8_t *buf, size_t buf_cap, size_t *out_len,
                                     const edgesync_message_t *msg, uint32_t seq)
{
    return edgesync_flashq_encode_message_full(buf, buf_cap, out_len, msg->id, msg->destination,
                                                msg->priority, EDGESYNC_FLASHQ_STATE_PENDING, 0,
                                                msg->created_at_us, 0, 0, 0,
                                                msg->payload, msg->payload_len, seq);
}

bool edgesync_flashq_encode_update(uint8_t *buf, size_t buf_cap, size_t *out_len,
                                    edgesync_message_id_t id, edgesync_flashq_state_t state,
                                    uint32_t attempt_count, int64_t last_attempt_at_us,
                                    int64_t next_retry_at_us, int32_t last_error, uint32_t seq)
{
    edgesync_flashq_record_hdr_t hdr = {0};
    hdr.seq = seq;
    hdr.type = EDGESYNC_FLASHQ_REC_UPDATE;
    hdr.state = (uint8_t)state;
    hdr.message_id = id;
    hdr.attempt_count = attempt_count;
    hdr.last_attempt_at_us = last_attempt_at_us;
    hdr.next_retry_at_us = next_retry_at_us;
    hdr.last_error = last_error;

    return encode_common(buf, buf_cap, out_len, &hdr, NULL, 0, NULL, 0);
}

bool edgesync_flashq_encode_end(uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t seq)
{
    edgesync_flashq_record_hdr_t hdr = {0};
    hdr.seq = seq;
    hdr.type = EDGESYNC_FLASHQ_REC_END;

    return encode_common(buf, buf_cap, out_len, &hdr, NULL, 0, NULL, 0);
}

bool edgesync_flashq_decode_header(const uint8_t *buf, size_t buf_len, edgesync_flashq_record_hdr_t *out_hdr)
{
    if (buf_len < EDGESYNC_FLASHQ_HDR_SIZE) {
        return false;
    }

    edgesync_flashq_record_hdr_t hdr;
    memcpy(&hdr, buf, EDGESYNC_FLASHQ_HDR_SIZE);

    if (hdr.magic != EDGESYNC_FLASHQ_REC_MAGIC) {
        return false; /* erased/blank flash or garbage */
    }

    uint32_t expected_crc = hdr.header_crc;
    if (header_crc(&hdr) != expected_crc) {
        return false; /* torn or corrupted write */
    }

    *out_hdr = hdr;
    return true;
}

bool edgesync_flashq_verify_body(const edgesync_flashq_record_hdr_t *hdr, const uint8_t *body, size_t available_len)
{
    size_t body_bytes = (size_t)hdr->destination_len + hdr->body_len;
    if (available_len < body_bytes + sizeof(uint32_t)) {
        return false;
    }

    uint32_t expected = get_u32(body + body_bytes);
    uint32_t actual = edgesync_crc32(0, body, body_bytes);
    return expected == actual;
}
