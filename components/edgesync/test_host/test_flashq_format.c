#include "tap.h"
#include "edgesync_flashq_format.h"
#include <string.h>

static edgesync_message_t make_msg(void)
{
    edgesync_message_t m = {0};
    m.id = 0x0102030405060708ULL;
    strcpy(m.destination, "telemetry");
    m.priority = EDGESYNC_PRIORITY_HIGH;
    m.created_at_us = 123456789;
    static const char payload[] = "{\"t\":28.5}";
    m.payload = payload;
    m.payload_len = (uint32_t)strlen(payload);
    return m;
}

int main(void)
{
    uint8_t buf[512];
    size_t len = 0;

    /* --- MESSAGE roundtrip --- */
    edgesync_message_t m = make_msg();
    CHECK(edgesync_flashq_encode_message(buf, sizeof(buf), &len, &m, 7));
    CHECK(len == edgesync_flashq_record_total_size((uint16_t)strlen(m.destination), m.payload_len));
    CHECK(len % 4 == 0);

    edgesync_flashq_record_hdr_t hdr;
    CHECK(edgesync_flashq_decode_header(buf, len, &hdr));
    CHECK(hdr.seq == 7);
    CHECK(hdr.type == EDGESYNC_FLASHQ_REC_MESSAGE);
    CHECK(hdr.state == EDGESYNC_FLASHQ_STATE_PENDING);
    CHECK(hdr.priority == EDGESYNC_PRIORITY_HIGH);
    CHECK(hdr.message_id == m.id);
    CHECK(hdr.destination_len == strlen(m.destination));
    CHECK(hdr.body_len == m.payload_len);

    const uint8_t *body = buf + EDGESYNC_FLASHQ_HDR_SIZE;
    CHECK(edgesync_flashq_verify_body(&hdr, body, len - EDGESYNC_FLASHQ_HDR_SIZE));
    CHECK(memcmp(body, m.destination, hdr.destination_len) == 0);
    CHECK(memcmp(body + hdr.destination_len, m.payload, hdr.body_len) == 0);

    /* --- Corruption detection: flip a header byte --- */
    uint8_t corrupt_hdr[512];
    memcpy(corrupt_hdr, buf, len);
    corrupt_hdr[10] ^= 0xFF;
    edgesync_flashq_record_hdr_t bad_hdr;
    CHECK(edgesync_flashq_decode_header(corrupt_hdr, len, &bad_hdr) == false);

    /* --- Corruption detection: flip a body byte (header still valid) --- */
    uint8_t corrupt_body[512];
    memcpy(corrupt_body, buf, len);
    corrupt_body[EDGESYNC_FLASHQ_HDR_SIZE + 2] ^= 0xFF;
    edgesync_flashq_record_hdr_t ok_hdr;
    CHECK(edgesync_flashq_decode_header(corrupt_body, len, &ok_hdr));
    CHECK(edgesync_flashq_verify_body(&ok_hdr, corrupt_body + EDGESYNC_FLASHQ_HDR_SIZE, len - EDGESYNC_FLASHQ_HDR_SIZE) == false);

    /* --- Torn write: truncate mid-record --- */
    CHECK(edgesync_flashq_decode_header(buf, EDGESYNC_FLASHQ_HDR_SIZE - 1, &bad_hdr) == false);
    CHECK(edgesync_flashq_decode_header(buf, len, &ok_hdr));
    size_t required = (size_t)ok_hdr.destination_len + ok_hdr.body_len + sizeof(uint32_t);
    CHECK(edgesync_flashq_verify_body(&ok_hdr, buf + EDGESYNC_FLASHQ_HDR_SIZE, required - 1) == false);

    /* --- Blank/erased flash reads as 0xFF and must decode as "not a record" --- */
    uint8_t erased[64];
    memset(erased, 0xFF, sizeof(erased));
    CHECK(edgesync_flashq_decode_header(erased, sizeof(erased), &bad_hdr) == false);

    /* --- UPDATE record roundtrip (no body) --- */
    size_t ulen = 0;
    CHECK(edgesync_flashq_encode_update(buf, sizeof(buf), &ulen, m.id, EDGESYNC_FLASHQ_STATE_DEAD_LETTER,
                                         3, 111, 222, ESP_FAIL, 8));
    edgesync_flashq_record_hdr_t uhdr;
    CHECK(edgesync_flashq_decode_header(buf, ulen, &uhdr));
    CHECK(uhdr.type == EDGESYNC_FLASHQ_REC_UPDATE);
    CHECK(uhdr.state == EDGESYNC_FLASHQ_STATE_DEAD_LETTER);
    CHECK(uhdr.attempt_count == 3);
    CHECK(uhdr.destination_len == 0);
    CHECK(uhdr.body_len == 0);
    CHECK(edgesync_flashq_verify_body(&uhdr, buf + EDGESYNC_FLASHQ_HDR_SIZE, ulen - EDGESYNC_FLASHQ_HDR_SIZE));

    /* --- END marker --- */
    size_t elen = 0;
    CHECK(edgesync_flashq_encode_end(buf, sizeof(buf), &elen, 9));
    edgesync_flashq_record_hdr_t ehdr;
    CHECK(edgesync_flashq_decode_header(buf, elen, &ehdr));
    CHECK(ehdr.type == EDGESYNC_FLASHQ_REC_END);

    /* --- Buffer-too-small rejects cleanly --- */
    size_t small_len;
    CHECK(edgesync_flashq_encode_message(buf, 4, &small_len, &m, 1) == false);

    TAP_REPORT();
}
