#include "tap.h"
#include "edgesync_retry.h"

int main(void)
{
    edgesync_retry_config_t cfg = {
        .initial_delay_ms = 1000,
        .max_delay_ms = 300000,
        .multiplier = 2,
        .jitter = false,
        .max_attempts = 0,
    };

    /* Matches the spec example: attempt 1 immediate, 2 -> ~1s, 3 -> ~2s, 4 -> ~4s, 5 -> ~8s. */
    CHECK(edgesync_retry_compute_delay_ms(&cfg, 0, 0) == 0);
    CHECK(edgesync_retry_compute_delay_ms(&cfg, 1, 0) == 1000);
    CHECK(edgesync_retry_compute_delay_ms(&cfg, 2, 0) == 2000);
    CHECK(edgesync_retry_compute_delay_ms(&cfg, 3, 0) == 4000);
    CHECK(edgesync_retry_compute_delay_ms(&cfg, 4, 0) == 8000);

    /* Clamps to max_delay_ms and never overflows for huge attempt counts. */
    CHECK(edgesync_retry_compute_delay_ms(&cfg, 40, 0) == 300000);
    CHECK(edgesync_retry_compute_delay_ms(&cfg, 4000000000u, 0) == 300000);

    /* Jitter: equal-jitter formula stays within [base/2, base]. */
    cfg.jitter = true;
    for (uint32_t attempt = 1; attempt <= 6; attempt++) {
        uint32_t base;
        {
            edgesync_retry_config_t nojit = cfg;
            nojit.jitter = false;
            base = edgesync_retry_compute_delay_ms(&nojit, attempt, 0);
        }
        for (uint32_t r = 0; r < 5; r++) {
            uint32_t d = edgesync_retry_compute_delay_ms(&cfg, attempt, r * 104729u);
            CHECK(d >= base / 2);
            CHECK(d <= cfg.max_delay_ms);
        }
    }

    /* max_attempts semantics. */
    edgesync_retry_config_t limited = cfg;
    limited.max_attempts = 3;
    CHECK(edgesync_retry_should_retry(&limited, 0) == true);
    CHECK(edgesync_retry_should_retry(&limited, 2) == true);
    CHECK(edgesync_retry_should_retry(&limited, 3) == false);

    edgesync_retry_config_t unlimited = cfg;
    unlimited.max_attempts = 0;
    CHECK(edgesync_retry_should_retry(&unlimited, 1000000) == true);

    /* apply_defaults only fills zeroed fields. */
    edgesync_retry_config_t d = {0};
    edgesync_retry_apply_defaults(&d, 1500, 60000, 2);
    CHECK(d.initial_delay_ms == 1500);
    CHECK(d.max_delay_ms == 60000);
    CHECK(d.multiplier == 2);

    edgesync_retry_config_t custom = { .initial_delay_ms = 250, .max_delay_ms = 0, .multiplier = 3, .jitter = false, .max_attempts = 0 };
    edgesync_retry_apply_defaults(&custom, 1500, 60000, 2);
    CHECK(custom.initial_delay_ms == 250);
    CHECK(custom.max_delay_ms == 60000);
    CHECK(custom.multiplier == 3);

    TAP_REPORT();
}
