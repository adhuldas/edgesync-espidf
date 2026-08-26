#include "edgesync_crc32.h"

/* Bit-wise CRC32 (IEEE 802.3 polynomial, reflected 0xEDB88320). No lookup
 * table is used to keep this out of flash-cost/RAM budget concerns; record
 * headers/bodies are small (tens of bytes to a few KB) so throughput is not
 * a bottleneck compared to the flash write itself. */
uint32_t edgesync_crc32(uint32_t seed, const void *data, size_t len)
{
    uint32_t crc = ~seed;
    const uint8_t *p = (const uint8_t *)data;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}
