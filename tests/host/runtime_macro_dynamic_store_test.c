/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>

#include "../../src/runtime_macro_ascii.c"
#include "../../src/runtime_macro_dynamic.c"

static int executor_start_result;
static unsigned int executor_start_calls;

int zmk_runtime_macro_executor_start(const uint8_t *text, size_t length) {
    (void)text;
    (void)length;
    executor_start_calls++;
    return executor_start_result;
}

int64_t host_uptime;

int host_work_schedule(struct k_work_delayable *work, k_timeout_t delay,
                       bool reschedule) {
    if (work->scheduled && !reschedule) {
        return -EBUSY;
    }

    work->scheduled = true;
    work->delay = delay;
    return 0;
}

static void run_ttl_work(void) {
    runtime_macro_dynamic_ttl_work.scheduled = false;
    runtime_macro_dynamic_ttl_work.work.handler(
        &runtime_macro_dynamic_ttl_work.work);
}

static bool all_zero(const uint8_t *buffer, size_t length) {
    for (size_t index = 0U; index < length; index++) {
        if (buffer[index] != 0U) {
            return false;
        }
    }

    return true;
}

static void assert_committed(const uint8_t *expected, size_t length) {
    assert(runtime_macro_dynamic_state.committed_valid);
    assert(runtime_macro_dynamic_state.committed_length == length);
    assert(memcmp(runtime_macro_dynamic_state.committed, expected, length) == 0);
}

static void assert_committed_empty(void) {
    assert(!runtime_macro_dynamic_state.committed_valid);
    assert(runtime_macro_dynamic_state.committed_length == 0U);
    assert(all_zero(runtime_macro_dynamic_state.committed,
                    sizeof(runtime_macro_dynamic_state.committed)));
}

static void assert_staging_empty(void) {
    assert(!runtime_macro_dynamic_state.staging_active);
    assert(runtime_macro_dynamic_state.staging_expected_length == 0U);
    assert(runtime_macro_dynamic_state.staging_received == 0U);
    assert(runtime_macro_dynamic_state.staging_ttl_seconds == 0U);
    assert(runtime_macro_dynamic_state.staging_consume_on_accept);
    assert(all_zero(runtime_macro_dynamic_state.staging,
                    sizeof(runtime_macro_dynamic_state.staging)));
}

static void commit_text_with_policy(const uint8_t *text, size_t length,
                                    uint32_t ttl_seconds,
                                    bool consume_on_accept) {
    assert(zmk_runtime_macro_dynamic_begin_with_options(
               length, ttl_seconds, consume_on_accept) == 0);
    assert(zmk_runtime_macro_dynamic_append(0U, text, length) == 0);
    assert_committed(text, length);
}

static void commit_text(const uint8_t *text, size_t length,
                        uint32_t ttl_seconds) {
    commit_text_with_policy(text, length, ttl_seconds, true);
}

static void test_reset_and_lengths(void) {
    const size_t lengths[] = {1U, 22U, 23U, 255U, 256U};
    uint8_t text[ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN];

    memset(text, 'A', sizeof(text));
    zmk_runtime_macro_dynamic_reset();
    assert_committed_empty();
    assert_staging_empty();

    for (size_t index = 0U; index < sizeof(lengths) / sizeof(lengths[0]);
         index++) {
        zmk_runtime_macro_dynamic_reset();
        assert(zmk_runtime_macro_dynamic_begin(lengths[index],
                                               ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
        assert(zmk_runtime_macro_dynamic_append(0U, text, lengths[index]) == 0);
        assert_committed(text, lengths[index]);
        assert_staging_empty();
    }

    zmk_runtime_macro_dynamic_reset();
    assert(zmk_runtime_macro_dynamic_begin(0U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) ==
           -EINVAL);
    assert(zmk_runtime_macro_dynamic_begin(257U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) ==
           -EINVAL);
    assert(zmk_runtime_macro_dynamic_begin(1U, 0U) == -EINVAL);
    assert(zmk_runtime_macro_dynamic_begin(1U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TTL_SECONDS + 1U) ==
           -EINVAL);
    assert_committed_empty();
    assert_staging_empty();
}

static void test_allowed_and_rejected_bytes(void) {
    uint8_t allowed[0x7f - 0x20 + 3];
    size_t allowed_length = 0U;

    for (uint16_t byte = 0x20U; byte <= 0x7eU; byte++) {
        allowed[allowed_length++] = (uint8_t)byte;
    }
    allowed[allowed_length++] = '\n';
    allowed[allowed_length++] = '\t';
    allowed[allowed_length++] = '\b';

    zmk_runtime_macro_dynamic_reset();
    commit_text(allowed, allowed_length,
                ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS);

    const uint8_t invalid[] = {0x00U, 0x01U, 0x07U, 0x0bU, 0x0cU,
                               0x1fU, 0x7fU, 0x80U, 0xffU};
    const uint8_t old[] = "old";

    for (size_t index = 0U; index < sizeof(invalid); index++) {
        assert(zmk_runtime_macro_dynamic_begin(1U,
                                               ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
        assert(zmk_runtime_macro_dynamic_append(0U, &invalid[index], 1U) ==
               -EINVAL);
        assert_committed(allowed, allowed_length);
        assert_staging_empty();
    }

    (void)old;
}

static void test_staging_and_atomic_commit(void) {
    const uint8_t old[] = "old";
    const uint8_t first[] = "new";
    const uint8_t final[] = "12";
    const uint8_t replacement[] = "AB";
    const uint8_t replacement_final[] = "CD";

    zmk_runtime_macro_dynamic_reset();
    commit_text(old, sizeof(old) - 1U,
                ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS);

    assert(zmk_runtime_macro_dynamic_begin(5U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
    assert(zmk_runtime_macro_dynamic_append(0U, first, sizeof(first) - 1U) ==
           0);
    assert_committed(old, sizeof(old) - 1U);
    assert(runtime_macro_dynamic_state.staging_received == 3U);
    assert(zmk_runtime_macro_dynamic_append(3U, final, sizeof(final) - 1U) ==
           0);
    assert_committed((const uint8_t *)"new12", 5U);
    assert_staging_empty();

    assert(zmk_runtime_macro_dynamic_begin(4U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
    assert(zmk_runtime_macro_dynamic_append(0U, replacement,
                                            sizeof(replacement) - 1U) == 0);
    assert(runtime_macro_dynamic_state.staging_received == 2U);
    assert(zmk_runtime_macro_dynamic_begin(2U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
    assert(runtime_macro_dynamic_state.staging_expected_length == 2U);
    assert(runtime_macro_dynamic_state.staging_received == 0U);
    assert(all_zero(runtime_macro_dynamic_state.staging,
                    sizeof(runtime_macro_dynamic_state.staging)));
    assert(zmk_runtime_macro_dynamic_append(0U, replacement_final,
                                            sizeof(replacement_final) - 1U) == 0);
    assert_committed((const uint8_t *)"CD", 2U);
    assert(all_zero(runtime_macro_dynamic_state.committed + 2U,
                    sizeof(runtime_macro_dynamic_state.committed) - 2U));

    assert(zmk_runtime_macro_dynamic_begin(4U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
    assert(zmk_runtime_macro_dynamic_append(1U, replacement, 1U) == -EINVAL);
    assert_committed((const uint8_t *)"CD", 2U);
    assert_staging_empty();
    assert(zmk_runtime_macro_dynamic_append(0U, replacement, 1U) == -ENOENT);
    assert_committed((const uint8_t *)"CD", 2U);

    assert(zmk_runtime_macro_dynamic_begin(4U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
    assert(zmk_runtime_macro_dynamic_append(0U, replacement, 3U) == -EINVAL);
    assert_committed((const uint8_t *)"CD", 2U);
    assert_staging_empty();

    assert(zmk_runtime_macro_dynamic_begin(4U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
    assert(zmk_runtime_macro_dynamic_append(0U, NULL, 1U) == -EINVAL);
    assert_committed((const uint8_t *)"CD", 2U);
    assert_staging_empty();
}

static void test_execution_consumption_policy(void) {
    const uint8_t text[] = "repeatable";

    zmk_runtime_macro_dynamic_reset();
    executor_start_result = 0;
    executor_start_calls = 0U;
    commit_text(text, sizeof(text) - 1U,
                ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS);
    assert(runtime_macro_dynamic_state.committed_consume_on_accept);
    assert(zmk_runtime_macro_dynamic_execute() == 0);
    assert(executor_start_calls == 1U);
    assert_committed_empty();

    zmk_runtime_macro_dynamic_reset();
    commit_text_with_policy(text, sizeof(text) - 1U,
                            ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS,
                            false);
    assert(!runtime_macro_dynamic_state.committed_consume_on_accept);
    assert(zmk_runtime_macro_dynamic_execute() == 0);
    assert(executor_start_calls == 2U);
    assert_committed(text, sizeof(text) - 1U);

    executor_start_result = -EBUSY;
    assert(zmk_runtime_macro_dynamic_execute() == -EBUSY);
    assert_committed(text, sizeof(text) - 1U);
}

static void test_ttl_and_generation(void) {
    const uint8_t first[] = "A";
    const uint8_t second[] = "B";
    uint32_t old_generation;

    zmk_runtime_macro_dynamic_reset();
    host_uptime = 1000;
    commit_text(first, sizeof(first) - 1U,
                ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS);
    assert(runtime_macro_dynamic_state.ttl_deadline_ms == 301000);
    assert(runtime_macro_dynamic_ttl_work.delay == 300000);
    old_generation = runtime_macro_dynamic_state.ttl_generation;

    host_uptime = 300999;
    run_ttl_work();
    assert_committed(first, sizeof(first) - 1U);
    assert(runtime_macro_dynamic_ttl_work.scheduled);
    assert(runtime_macro_dynamic_ttl_work.delay == 1);

    host_uptime = 301000;
    run_ttl_work();
    assert_committed_empty();
    assert_staging_empty();
    assert(runtime_macro_dynamic_state.ttl_deadline_ms == 0);
    assert(!runtime_macro_dynamic_ttl_work.scheduled);

    host_uptime = 5000;
    commit_text(first, sizeof(first) - 1U, 1U);
    assert(runtime_macro_dynamic_state.ttl_deadline_ms == 6000);
    assert(runtime_macro_dynamic_ttl_work.delay == 1000);

    host_uptime = 500;
    commit_text(first, sizeof(first) - 1U,
                ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TTL_SECONDS);
    assert(runtime_macro_dynamic_state.ttl_deadline_ms == 86400500);
    assert(runtime_macro_dynamic_ttl_work.delay == 86400000);
    assert(runtime_macro_dynamic_state.ttl_generation != old_generation);

    host_uptime = 6000;
    run_ttl_work();
    assert_committed(first, sizeof(first) - 1U);
    assert(runtime_macro_dynamic_ttl_work.scheduled);

    host_uptime = 86400500;
    run_ttl_work();
    assert_committed_empty();
    assert_staging_empty();

    zmk_runtime_macro_dynamic_reset();
    host_uptime = 0;
    commit_text(first, sizeof(first) - 1U, 1U);
    old_generation = runtime_macro_dynamic_state.ttl_generation;
    assert(zmk_runtime_macro_dynamic_begin(1U, 86400U) == 0);
    host_uptime = 500;
    assert(zmk_runtime_macro_dynamic_append(0U, second, 1U) == 0);
    assert(runtime_macro_dynamic_state.ttl_generation > old_generation);
    host_uptime = 1000;
    run_ttl_work();
    assert_committed(second, sizeof(second) - 1U);
}

static void test_clear_and_zeroize(void) {
    const uint8_t text[] = "clear me";

    zmk_runtime_macro_dynamic_reset();
    commit_text(text, sizeof(text) - 1U,
                ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS);
    assert(zmk_runtime_macro_dynamic_begin(4U,
                                           ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
    assert(zmk_runtime_macro_dynamic_append(0U, text, 2U) == 0);
    zmk_runtime_macro_dynamic_clear();
    assert_committed_empty();
    assert_staging_empty();
    assert(!runtime_macro_dynamic_ttl_work.scheduled);
    zmk_runtime_macro_dynamic_clear();
    assert_committed_empty();
    assert_staging_empty();
}

struct race_context {
    const uint8_t *text;
    size_t length;
};

static void *append_final(void *arg) {
    const struct race_context *context = arg;
    const int err =
        zmk_runtime_macro_dynamic_append(255U, context->text, context->length);
    assert(err == 0 || err == -ENOENT);
    return NULL;
}

static void *clear_store(void *arg) {
    (void)arg;
    zmk_runtime_macro_dynamic_clear();
    return NULL;
}

static void test_clear_vs_final_append(void) {
    uint8_t text[256];
    uint8_t expected[256];
    const uint8_t final_byte = 'Z';
    const struct race_context context = {.text = &final_byte, .length = 1U};

    memset(text, 'Q', sizeof(text));
    memcpy(expected, text, sizeof(expected));
    expected[sizeof(expected) - 1U] = final_byte;
    for (size_t iteration = 0U; iteration < 100U; iteration++) {
        pthread_t append_thread;
        pthread_t clear_thread;

        zmk_runtime_macro_dynamic_reset();
        assert(zmk_runtime_macro_dynamic_begin(sizeof(text), 300U) == 0);
        assert(zmk_runtime_macro_dynamic_append(0U, text, sizeof(text) - 1U) ==
               0);
        assert(pthread_create(&append_thread, NULL, append_final,
                              (void *)&context) == 0);
        assert(pthread_create(&clear_thread, NULL, clear_store, NULL) == 0);
        assert(pthread_join(append_thread, NULL) == 0);
        assert(pthread_join(clear_thread, NULL) == 0);

        assert(!runtime_macro_dynamic_state.staging_active);
        assert(runtime_macro_dynamic_state.committed_length == 0U ||
               runtime_macro_dynamic_state.committed_length == sizeof(text));
        if (runtime_macro_dynamic_state.committed_length == 0U) {
            assert_committed_empty();
        } else {
            assert_committed(expected, sizeof(expected));
        }
    }
}

int main(void) {
    test_reset_and_lengths();
    test_allowed_and_rejected_bytes();
    test_staging_and_atomic_commit();
    test_execution_consumption_policy();
    test_ttl_and_generation();
    test_clear_and_zeroize();
    test_clear_vs_final_append();
    puts("runtime macro dynamic store: PASS");
    return 0;
}
