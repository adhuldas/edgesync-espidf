#include "edgesync_id.h"
#include "esp_random.h"

edgesync_message_id_t edgesync_id_generate(void)
{
    uint64_t id = 0;
    /* esp_fill_random() draws from the hardware TRNG (RF subsystem noise,
     * amplified when Wi-Fi/BT is active, or the SAR ADC otherwise) - suitable
     * for a collision-resistant identifier, not for cryptographic secrets. */
    do {
        esp_fill_random(&id, sizeof(id));
    } while (id == EDGESYNC_MESSAGE_ID_INVALID);

    return (edgesync_message_id_t)id;
}

void edgesync_id_to_hex(edgesync_message_id_t id, char out[17])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        out[i] = digits[id & 0xF];
        id >>= 4;
    }
    out[16] = '\0';
}
