/**
 * @file edgesync_retry.h
 * @brief Exponential backoff with jitter, computed as a pure function.
 *
 * Deliberately takes randomness as an input parameter (instead of calling
 * esp_random() internally) so the core algorithm is deterministic and can be
 * unit tested on the host without ESP-IDF - see test_host/test_retry.c and
 * test/test_retry.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t initial_delay_ms;
    uint32_t max_delay_ms;
    uint32_t multiplier;   /**< >= 2. */
    bool jitter;
    uint32_t max_attempts; /**< 0 = unlimited. */
} edgesync_retry_config_t;

/** Normalize a possibly-zeroed config in place, applying the given defaults for any zero field. */
void edgesync_retry_apply_defaults(edgesync_retry_config_t *cfg,
                                    uint32_t default_initial_ms,
                                    uint32_t default_max_ms,
                                    uint32_t default_multiplier);

/**
 * @brief Compute the delay (in ms) before the next attempt.
 *
 * @param cfg Retry configuration (already defaulted).
 * @param attempt_count Number of attempts already made (0 before the first attempt).
 * @param random_u32 A fresh random value supplied by the caller, consumed for jitter only.
 *
 * @return 0 for the first attempt (attempt_count == 0). Otherwise
 *         min(max_delay_ms, initial_delay_ms * multiplier^(attempt_count-1)),
 *         with equal-jitter applied (delay/2 + rand(0, delay/2)) when
 *         cfg->jitter is true. Overflow-safe for arbitrarily large attempt_count.
 */
uint32_t edgesync_retry_compute_delay_ms(const edgesync_retry_config_t *cfg,
                                          uint32_t attempt_count,
                                          uint32_t random_u32);

/** True if another attempt should be made given attempts already made. */
bool edgesync_retry_should_retry(const edgesync_retry_config_t *cfg, uint32_t attempt_count);

#ifdef __cplusplus
}
#endif
