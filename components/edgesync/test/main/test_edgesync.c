/**
 * @file test_edgesync.c
 * @brief On-target integration tests: things that need real flash and FreeRTOS.
 *
 * Pure-logic modules (CRC, retry math, record encode/decode) are covered by
 * test_host/ instead - see CONTRIBUTING.md.
 */
#include <string.h>

#include "unity.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "edgesync/edgesync.h"
#include "edgesync_storage_flashq.h"

#define TEST_ESP_OK(expr) TEST_ASSERT_EQUAL(ESP_OK, (expr))

/* ------------------------------------------------------------------------
 * Fixture: the "edgesync" data partition is shared by every test case, so
 * each one starts from a fully erased partition.
 * ---------------------------------------------------------------------- */

void setUp(void)
{
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "edgesync");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ESP_OK(esp_partition_erase_range(p, 0, p->size));
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------
 * Fake transports
 * ---------------------------------------------------------------------- */

/** Returns a scripted sequence of results, one per call (the last result repeats if over-called). */
typedef struct {
    const edgesync_delivery_result_t *results;
    size_t result_count;
    volatile size_t call_count;
} scripted_transport_ctx_t;

static edgesync_delivery_result_t scripted_deliver(void *vctx, const edgesync_message_t *msg, int *status)
{
    (void)msg;
    scripted_transport_ctx_t *ctx = vctx;
    if (status != NULL) {
        *status = 0;
    }
    size_t idx = (ctx->call_count < ctx->result_count) ? ctx->call_count : ctx->result_count - 1;
    ctx->call_count++;
    return ctx->results[idx];
}

static const edgesync_transport_ops_t scripted_ops = { .deliver = scripted_deliver };

/** Blocks well past this test file's assertions, to hold a message IN_FLIGHT deterministically. */
static edgesync_delivery_result_t blocking_deliver(void *vctx, const edgesync_message_t *msg, int *status)
{
    (void)vctx; (void)msg;
    if (status != NULL) {
        *status = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
    return EDGESYNC_DELIVERY_SUCCESS;
}

static const edgesync_transport_ops_t blocking_ops = { .deliver = blocking_deliver };

/* ------------------------------------------------------------------------
 * Event capture
 * ---------------------------------------------------------------------- */

static QueueHandle_t s_events;

static void capture_event_cb(edgesync_event_id_t event, const edgesync_message_t *msg, void *ctx)
{
    (void)msg; (void)ctx;
    xQueueSend(s_events, &event, 0);
}

/** Polls the event queue until `expected` is seen or the timeout elapses. */
static bool wait_for_event(edgesync_event_id_t expected, TickType_t timeout_ticks)
{
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;
    while (xTaskGetTickCount() < deadline) {
        edgesync_event_id_t ev;
        TickType_t remaining = deadline - xTaskGetTickCount();
        if (xQueueReceive(s_events, &ev, remaining) == pdTRUE && ev == expected) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Black-box tests (public API only)
 * ---------------------------------------------------------------------- */

TEST_CASE("publish is durably delivered by a successful transport", "[edgesync]")
{
    s_events = xQueueCreate(16, sizeof(edgesync_event_id_t));

    edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
    config.transport_ops = &scripted_ops;
    const edgesync_delivery_result_t results[] = { EDGESYNC_DELIVERY_SUCCESS };
    scripted_transport_ctx_t fake = { .results = results, .result_count = 1 };
    config.transport_ctx = &fake;
    config.event_cb = capture_event_cb;

    edgesync_handle_t handle;
    TEST_ESP_OK(edgesync_init(&config, &handle));
    TEST_ESP_OK(edgesync_start(handle));
    TEST_ESP_OK(edgesync_publish(handle, "telemetry", "x", 1, NULL));

    TEST_ASSERT_TRUE(wait_for_event(EDGESYNC_EVENT_DELIVERED, pdMS_TO_TICKS(2000)));

    edgesync_stats_t stats;
    TEST_ESP_OK(edgesync_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, stats.pending_messages);
    TEST_ASSERT_EQUAL_UINT64(1, stats.successful_deliveries);

    TEST_ESP_OK(edgesync_stop(handle));
    TEST_ESP_OK(edgesync_deinit(handle));
    vQueueDelete(s_events);
}

TEST_CASE("retryable failures are retried with backoff until success", "[edgesync]")
{
    s_events = xQueueCreate(16, sizeof(edgesync_event_id_t));

    edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
    config.transport_ops = &scripted_ops;
    const edgesync_delivery_result_t results[] = {
        EDGESYNC_DELIVERY_RETRYABLE_FAILURE, EDGESYNC_DELIVERY_RETRYABLE_FAILURE, EDGESYNC_DELIVERY_SUCCESS,
    };
    scripted_transport_ctx_t fake = { .results = results, .result_count = 3 };
    config.transport_ctx = &fake;
    config.event_cb = capture_event_cb;
    config.retry_initial_ms = 20;
    config.retry_max_ms = 100;
    config.retry_jitter = false;

    edgesync_handle_t handle;
    TEST_ESP_OK(edgesync_init(&config, &handle));
    TEST_ESP_OK(edgesync_start(handle));
    TEST_ESP_OK(edgesync_publish(handle, "telemetry", "x", 1, NULL));

    TEST_ASSERT_TRUE(wait_for_event(EDGESYNC_EVENT_RETRY, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_TRUE(wait_for_event(EDGESYNC_EVENT_RETRY, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_TRUE(wait_for_event(EDGESYNC_EVENT_DELIVERED, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_EQUAL_size_t(3, fake.call_count);

    TEST_ESP_OK(edgesync_stop(handle));
    TEST_ESP_OK(edgesync_deinit(handle));
    vQueueDelete(s_events);
}

TEST_CASE("a permanent failure dead-letters immediately", "[edgesync]")
{
    s_events = xQueueCreate(16, sizeof(edgesync_event_id_t));

    edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
    config.transport_ops = &scripted_ops;
    const edgesync_delivery_result_t results[] = { EDGESYNC_DELIVERY_PERMANENT_FAILURE };
    scripted_transport_ctx_t fake = { .results = results, .result_count = 1 };
    config.transport_ctx = &fake;
    config.event_cb = capture_event_cb;

    edgesync_handle_t handle;
    TEST_ESP_OK(edgesync_init(&config, &handle));
    TEST_ESP_OK(edgesync_start(handle));
    TEST_ESP_OK(edgesync_publish(handle, "telemetry", "x", 1, NULL));

    TEST_ASSERT_TRUE(wait_for_event(EDGESYNC_EVENT_DEAD_LETTER, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_EQUAL_size_t(1, fake.call_count);

    edgesync_stats_t stats;
    TEST_ESP_OK(edgesync_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, stats.dead_letter_messages);

    TEST_ESP_OK(edgesync_stop(handle));
    TEST_ESP_OK(edgesync_deinit(handle));
    vQueueDelete(s_events);
}

TEST_CASE("retry_max_attempts dead-letters once the limit is reached", "[edgesync]")
{
    s_events = xQueueCreate(16, sizeof(edgesync_event_id_t));

    edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
    config.transport_ops = &scripted_ops;
    const edgesync_delivery_result_t results[] = {
        EDGESYNC_DELIVERY_RETRYABLE_FAILURE, EDGESYNC_DELIVERY_RETRYABLE_FAILURE, EDGESYNC_DELIVERY_RETRYABLE_FAILURE,
    };
    scripted_transport_ctx_t fake = { .results = results, .result_count = 3 };
    config.transport_ctx = &fake;
    config.event_cb = capture_event_cb;
    config.retry_initial_ms = 10;
    config.retry_max_ms = 50;
    config.retry_jitter = false;
    config.retry_max_attempts = 2;

    edgesync_handle_t handle;
    TEST_ESP_OK(edgesync_init(&config, &handle));
    TEST_ESP_OK(edgesync_start(handle));
    TEST_ESP_OK(edgesync_publish(handle, "telemetry", "x", 1, NULL));

    TEST_ASSERT_TRUE(wait_for_event(EDGESYNC_EVENT_DEAD_LETTER, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_EQUAL_size_t(2, fake.call_count); /* stopped after the 2nd attempt, not the 3rd */

    TEST_ESP_OK(edgesync_stop(handle));
    TEST_ESP_OK(edgesync_deinit(handle));
    vQueueDelete(s_events);
}

TEST_CASE("overflow policy REJECT_NEW rejects publish once the queue is full", "[edgesync]")
{
    edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
    config.transport_ops = &blocking_ops; /* holds the first message IN_FLIGHT for the test's duration */
    config.max_queue_size = 2;
    config.overflow_policy = EDGESYNC_OVERFLOW_REJECT_NEW;

    edgesync_handle_t handle;
    TEST_ESP_OK(edgesync_init(&config, &handle));
    TEST_ESP_OK(edgesync_start(handle));

    TEST_ESP_OK(edgesync_publish(handle, "telemetry", "x", 1, NULL));
    TEST_ESP_OK(edgesync_publish(handle, "telemetry", "x", 1, NULL));
    TEST_ASSERT_EQUAL(EDGESYNC_ERR_QUEUE_FULL, edgesync_publish(handle, "telemetry", "x", 1, NULL));

    TEST_ESP_OK(edgesync_stop(handle)); /* waits for the blocking delivery to finish */
    TEST_ESP_OK(edgesync_deinit(handle));
}

TEST_CASE("overflow policy DROP_OLDEST makes room instead of rejecting", "[edgesync]")
{
    s_events = xQueueCreate(16, sizeof(edgesync_event_id_t));

    edgesync_config_t config = EDGESYNC_DEFAULT_CONFIG();
    config.transport_ops = &blocking_ops;
    config.max_queue_size = 2;
    config.overflow_policy = EDGESYNC_OVERFLOW_DROP_OLDEST;
    config.event_cb = capture_event_cb;

    edgesync_handle_t handle;
    TEST_ESP_OK(edgesync_init(&config, &handle));
    TEST_ESP_OK(edgesync_start(handle));

    TEST_ESP_OK(edgesync_publish(handle, "telemetry", "x", 1, NULL));
    TEST_ESP_OK(edgesync_publish(handle, "telemetry", "x", 1, NULL));
    /* Queue is full, but the policy drops the oldest PENDING message instead of rejecting. */
    TEST_ESP_OK(edgesync_publish(handle, "telemetry", "x", 1, NULL));
    TEST_ASSERT_TRUE(wait_for_event(EDGESYNC_EVENT_QUEUE_FULL, pdMS_TO_TICKS(1000)));

    TEST_ESP_OK(edgesync_stop(handle));
    TEST_ESP_OK(edgesync_deinit(handle));
    vQueueDelete(s_events);
}

/* ------------------------------------------------------------------------
 * White-box test: crash safety of the flash-queue backend directly.
 * ---------------------------------------------------------------------- */

TEST_CASE("an IN_FLIGHT message is requeued as PENDING after a simulated crash", "[edgesync]")
{
    edgesync_flashq_config_t cfg = {
        .partition_label = "edgesync",
        .max_messages = 8,
        .max_message_size = 256,
    };

    const edgesync_storage_ops_t *ops;
    void *ctx;
    TEST_ESP_OK(edgesync_flashq_create(&cfg, &ops, &ctx));
    TEST_ESP_OK(ops->init(ctx));
    TEST_ESP_OK(ops->recover(ctx));

    edgesync_message_t msg = {0};
    msg.id = 0x1122334455667788ULL;
    strcpy(msg.destination, "telemetry");
    msg.priority = EDGESYNC_PRIORITY_NORMAL;
    msg.payload = "x";
    msg.payload_len = 1;
    TEST_ESP_OK(ops->enqueue(ctx, &msg));

    edgesync_message_t claimed = {0};
    TEST_ESP_OK(ops->claim_next(ctx, 0, &claimed));
    TEST_ASSERT_EQUAL(EDGESYNC_MSG_IN_FLIGHT, claimed.state);
    TEST_ASSERT_EQUAL_UINT32(1, claimed.attempt_count);

    /* Simulate a crash mid-delivery: tear down without any further durable
     * update, then reopen the same partition as a brand new instance. */
    ops->close(ctx);
    edgesync_flashq_destroy(ctx);

    TEST_ESP_OK(edgesync_flashq_create(&cfg, &ops, &ctx));
    TEST_ESP_OK(ops->init(ctx));
    TEST_ESP_OK(ops->recover(ctx));

    edgesync_message_t reclaimed = {0};
    TEST_ESP_OK(ops->claim_next(ctx, 0, &reclaimed));
    TEST_ASSERT_EQUAL_UINT64(msg.id, reclaimed.id);
    TEST_ASSERT_EQUAL_UINT32(2, reclaimed.attempt_count); /* recovered as PENDING, then claimed again */

    ops->close(ctx);
    edgesync_flashq_destroy(ctx);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
