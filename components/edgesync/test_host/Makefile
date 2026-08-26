# Host-side unit tests for the hardware-independent core logic
# (CRC32, retry backoff, flash record format). These compile with the
# system C compiler and a couple of mock headers - no ESP-IDF required -
# so they can run in CI or locally on any machine. They complement (do not
# replace) the on-target tests under ../test which exercise the real
# esp_partition-backed storage backend and full ESP-IDF integration.
CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g \
         -I. -Imocks -I../src -I../include

SRC_DIR = ../src

.PHONY: all test clean

all: test

test: test_crc32 test_retry test_flashq_format
	./test_crc32
	./test_retry
	./test_flashq_format

test_crc32: test_crc32.c $(SRC_DIR)/edgesync_crc32.c
	$(CC) $(CFLAGS) -o $@ $^

test_retry: test_retry.c $(SRC_DIR)/edgesync_retry.c
	$(CC) $(CFLAGS) -o $@ $^

test_flashq_format: test_flashq_format.c $(SRC_DIR)/edgesync_flashq_format.c $(SRC_DIR)/edgesync_crc32.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_crc32 test_retry test_flashq_format
