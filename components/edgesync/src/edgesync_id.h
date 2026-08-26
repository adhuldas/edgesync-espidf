/**
 * @file edgesync_id.h
 * @brief Message ID generation.
 */
#pragma once

#include "edgesync/edgesync_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Generate a new random, non-zero 64-bit message id (esp_fill_random-backed). */
edgesync_message_id_t edgesync_id_generate(void);

/** Format an id as 16 lowercase hex characters into out[17] (NUL-terminated). */
void edgesync_id_to_hex(edgesync_message_id_t id, char out[17]);

#ifdef __cplusplus
}
#endif
