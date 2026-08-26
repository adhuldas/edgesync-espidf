/* Tiny host-test assertion helper (no framework dependency). */
#pragma once
#include <stdio.h>
#include <stdlib.h>

static int tap_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            tap_failures++;                                                  \
        }                                                                    \
    } while (0)

#define TAP_REPORT()                                                         \
    do {                                                                     \
        if (tap_failures) {                                                  \
            fprintf(stderr, "%d check(s) failed\n", tap_failures);           \
            return 1;                                                        \
        }                                                                    \
        printf("OK: %s\n", __FILE__);                                       \
        return 0;                                                            \
    } while (0)
