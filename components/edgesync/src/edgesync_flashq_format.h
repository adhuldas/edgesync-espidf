/**
 * @file edgesync_flashq_format.h
 * @brief On-flash record format for the flash-queue storage backend.
 *
 * This header and its .c file contain no ESP-IDF/hardware dependencies
 * (only plain buffers) so the encode/decode/CRC logic - the part most
 * critical to get right for crash-consistency - can be exercised by host
 * unit tests without a device or QEMU. See STORAGE.md for the full format
 * rationale (ping-pong regions, append-only log, bit-clearing status bytes).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "edgesync/edgesync_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGESYNC_FLASHQ_REC_MAGIC   0x45445347u /* 'EDSG' */
#define EDGESYNC_FLASHQ_HDR_MAGIC   0x45445348u /* 'EDSH' - region header magic */

/* Region header status byte values. Chosen so that every legal transition
 * only clears bits (1 -> 0), which is the only thing NOR flash can do
 * without an erase cycle: 0xFF (erased) -> COMPACTING -> ACTIVE -> STALE. */
#define EDGESYNC_FLASHQ_STATUS_EMPTY      0xFFu
#define EDGESYNC_FLASHQ_STATUS_COMPACTING 0x07u
#define EDGESYNC_FLASHQ_STATUS_ACTIVE     0x03u
#define EDGESYNC_FLASHQ_STATUS_STALE      0x01u

typedef enum {
    EDGESYNC_FLASHQ_REC_MESSAGE = 1, /**< Full message: header + destination + payload. */
    EDGESYNC_FLASHQ_REC_UPDATE  = 2, /**< State-only update for an existing message id. */
    EDGESYNC_FLASHQ_REC_END     = 3, /**< Marks a freshly-compacted region as complete. */
} edgesync_flashq_record_type_t;

/** On-flash record state byte. A superset of edgesync_message_state_t: adds
 *  REMOVED for entries dropped by an overflow policy, which must never be
 *  surfaced through the public API but must still be tracked so recovery
 *  and compaction can reclaim their space. Values 0-3 are chosen to match
 *  edgesync_message_state_t numerically for convenience. */
typedef enum {
    EDGESYNC_FLASHQ_STATE_PENDING     = 0,
    EDGESYNC_FLASHQ_STATE_IN_FLIGHT   = 1,
    EDGESYNC_FLASHQ_STATE_DELIVERED   = 2,
    EDGESYNC_FLASHQ_STATE_DEAD_LETTER = 3,
    EDGESYNC_FLASHQ_STATE_REMOVED     = 4,
} edgesync_flashq_state_t;

#if defined(__GNUC__) || defined(__clang__)
#define EDGESYNC_PACKED __attribute__((packed))
#else
#define EDGESYNC_PACKED
#endif

typedef struct EDGESYNC_PACKED {
    uint32_t magic;
    uint32_t seq;
    uint8_t  type;             /* edgesync_flashq_record_type_t */
    uint8_t  state;            /* edgesync_flashq_state_t */
    uint8_t  priority;         /* edgesync_priority_t */
    uint8_t  reserved;
    uint64_t message_id;
    uint32_t attempt_count;
    int64_t  created_at_us;
    int64_t  last_attempt_at_us;
    int64_t  next_retry_at_us;
    int32_t  last_error;
    uint16_t destination_len;  /* 0 for UPDATE/END */
    uint32_t body_len;         /* payload length for MESSAGE; 0 for UPDATE/END */
    uint32_t header_crc;       /* CRC32 over all preceding bytes, with this field treated as 0 */
} edgesync_flashq_record_hdr_t;

#define EDGESYNC_FLASHQ_HDR_SIZE (sizeof(edgesync_flashq_record_hdr_t))

/** Region header (occupies the first bytes of the region's first sector). */
typedef struct EDGESYNC_PACKED {
    uint32_t magic;
    uint32_t generation;
    uint8_t  status;
    uint8_t  reserved[3];
} edgesync_flashq_region_hdr_t;

#define EDGESYNC_FLASHQ_REGION_HDR_SIZE (sizeof(edgesync_flashq_region_hdr_t))

/** Round `n` up to the next multiple of 4 (flash write alignment/portability). */
static inline size_t edgesync_flashq_round4(size_t n)
{
    return (n + 3u) & ~(size_t)3u;
}

/** Total bytes a record occupies on flash, including the trailing CRC and alignment padding. */
static inline size_t edgesync_flashq_record_total_size(uint16_t destination_len, uint32_t body_len)
{
    return edgesync_flashq_round4(EDGESYNC_FLASHQ_HDR_SIZE + destination_len + body_len + sizeof(uint32_t));
}

/**
 * @brief Encode a full MESSAGE record.
 *
 * @param buf Destination buffer.
 * @param buf_cap Capacity of `buf`.
 * @param out_len Set to the total encoded length (including padding) on success.
 * @return false if buf_cap is insufficient or destination_len exceeds EDGESYNC_MAX_DESTINATION_LEN.
 */
bool edgesync_flashq_encode_message(uint8_t *buf, size_t buf_cap, size_t *out_len,
                                     const edgesync_message_t *msg, uint32_t seq);

/**
 * @brief Encode a MESSAGE record with explicit state/attempt/timestamp fields.
 *
 * Used by edgesync_flashq_encode_message() for brand-new PENDING messages,
 * and directly by the compaction routine to re-serialize an existing
 * message's current state into a single consolidated record.
 */
bool edgesync_flashq_encode_message_full(uint8_t *buf, size_t buf_cap, size_t *out_len,
                                          edgesync_message_id_t id, const char *destination,
                                          edgesync_priority_t priority, edgesync_flashq_state_t state,
                                          uint32_t attempt_count, int64_t created_at_us,
                                          int64_t last_attempt_at_us, int64_t next_retry_at_us,
                                          int32_t last_error, const void *payload, uint32_t payload_len,
                                          uint32_t seq);

/** Encode a state-only UPDATE record (no destination/payload body). */
bool edgesync_flashq_encode_update(uint8_t *buf, size_t buf_cap, size_t *out_len,
                                    edgesync_message_id_t id, edgesync_flashq_state_t state,
                                    uint32_t attempt_count, int64_t last_attempt_at_us,
                                    int64_t next_retry_at_us, int32_t last_error, uint32_t seq);

/** Encode a compaction-complete END marker. */
bool edgesync_flashq_encode_end(uint8_t *buf, size_t buf_cap, size_t *out_len, uint32_t seq);

/**
 * @brief Validate and decode a record header at the start of `buf`.
 *
 * Returns false if there are not enough bytes, the magic does not match
 * (typically erased/blank flash), or the header CRC fails (torn/corrupt
 * write). Callers use this to find the log's write cursor during recovery:
 * the first position where this returns false is the end of the valid log.
 */
bool edgesync_flashq_decode_header(const uint8_t *buf, size_t buf_len, edgesync_flashq_record_hdr_t *out_hdr);

/**
 * @brief Validate the body CRC that follows a decoded header.
 *
 * `body` must point at (destination_len + body_len) bytes immediately
 * followed by the little-endian uint32 body CRC, i.e. buf + EDGESYNC_FLASHQ_HDR_SIZE.
 */
bool edgesync_flashq_verify_body(const edgesync_flashq_record_hdr_t *hdr, const uint8_t *body, size_t available_len);

#ifdef __cplusplus
}
#endif
