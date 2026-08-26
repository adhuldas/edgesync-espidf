#include "tap.h"
#include "edgesync_crc32.h"
#include <string.h>

int main(void)
{
    /* Known CRC32 test vector. */
    const char *s = "123456789";
    CHECK(edgesync_crc32(0, s, strlen(s)) == 0xCBF43926u);

    CHECK(edgesync_crc32(0, "", 0) == 0x00000000u);

    uint8_t data[] = {1, 2, 3, 4, 5};
    uint32_t c1 = edgesync_crc32(0, data, sizeof(data));
    data[2] ^= 0xFF; /* flip a byte */
    uint32_t c2 = edgesync_crc32(0, data, sizeof(data));
    CHECK(c1 != c2);

    TAP_REPORT();
}
