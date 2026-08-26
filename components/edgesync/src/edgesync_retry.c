#include "edgesync_retry.h"

void edgesync_retry_apply_defaults(edgesync_retry_config_t *cfg,
                                    uint32_t default_initial_ms,
                                    uint32_t default_max_ms,
                                    uint32_t default_multiplier)
{
    if (cfg->initial_delay_ms == 0) {
        cfg->initial_delay_ms = default_initial_ms;
    }
    if (cfg->max_delay_ms == 0) {
        cfg->max_delay_ms = default_max_ms;
    }
    if (cfg->multiplier < 2) {
        cfg->multiplier = default_multiplier;
    }
}

uint32_t edgesync_retry_compute_delay_ms(const edgesync_retry_config_t *cfg,
                                          uint32_t attempt_count,
                                          uint32_t random_u32)
{
    if (attempt_count == 0) {
        return 0; /* First attempt is always immediate. */
    }

    /* base = initial_delay_ms * multiplier^(attempt_count - 1), clamped to
     * max_delay_ms. Multiply iteratively and bail out the moment we would
     * meet/exceed the cap, to stay overflow-safe for large attempt counts. */
    uint64_t base = cfg->initial_delay_ms;
    for (uint32_t i = 0; i < attempt_count - 1; i++) {
        if (base >= cfg->max_delay_ms) {
            base = cfg->max_delay_ms;
            break;
        }
        base *= cfg->multiplier;
    }
    if (base > cfg->max_delay_ms) {
        base = cfg->max_delay_ms;
    }

    if (!cfg->jitter || base == 0) {
        return (uint32_t)base;
    }

    /* Equal jitter: half fixed, half random. Avoids the thundering-herd
     * effect of many devices retrying in lockstep while still keeping the
     * delay close to the intended backoff curve. */
    uint64_t half = base / 2;
    uint64_t jittered = half + (random_u32 % (half + 1));
    if (jittered > cfg->max_delay_ms) {
        jittered = cfg->max_delay_ms;
    }
    return (uint32_t)jittered;
}

bool edgesync_retry_should_retry(const edgesync_retry_config_t *cfg, uint32_t attempt_count)
{
    if (cfg->max_attempts == 0) {
        return true; /* unlimited */
    }
    return attempt_count < cfg->max_attempts;
}
