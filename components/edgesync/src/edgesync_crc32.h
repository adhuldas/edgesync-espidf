/**
 * @file edgesync_crc32.h
 * @brief Minimal CRC32 (IEEE 802.3) used for on-flash record integrity checks.
 *
 * Intentionally self-contained (no ESP-IDF dependency) so it can be unit
 * tested on the host; see test_host/.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Continue a CRC32 computation. Pass 0 as `seed` to start a new checksum. */
uint32_t edgesync_crc32(uint32_t seed, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
