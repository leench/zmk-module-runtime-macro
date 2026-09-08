/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zmk/runtime_macro.h>

#include "runtime_macro_dynamic_internal.h"
#include "runtime_macro_executor_internal.h"

static struct zmk_runtime_macro_dynamic_state
    runtime_macro_dynamic_state __attribute__((used));

K_MUTEX_DEFINE(runtime_macro_dynamic_mutex);

static void runtime_macro_dynamic_ttl_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(runtime_macro_dynamic_ttl_work,
                        runtime_macro_dynamic_ttl_work_handler);

_Static_assert(sizeof(runtime_macro_dynamic_state.committed) ==
                   ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN,
               "dynamic committed buffer size must match the fixed maximum");
_Static_assert(sizeof(runtime_macro_dynamic_state.staging) ==
                   ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN,
               "dynamic staging buffer size must match the fixed maximum");

static void runtime_macro_dynamic_zeroize(void *buffer, size_t length) {
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;

    while (length-- > 0U) {
        *bytes++ = 0U;
    }
}

static bool runtime_macro_dynamic_byte_is_valid(uint8_t byte) {
    uint32_t encoded;

    /* Use the same US-ASCII mapping that static macros validate and execute. */
    return zmk_runtime_macro_ascii_to_keycode(byte, &encoded) == 0;
}

static uint32_t runtime_macro_dynamic_next_generation(void) {
    runtime_macro_dynamic_state.ttl_generation++;
    if (runtime_macro_dynamic_state.ttl_generation == 0U) {
        runtime_macro_dynamic_state.ttl_generation++;
    }

    return runtime_macro_dynamic_state.ttl_generation;
}

static void runtime_macro_dynamic_cancel_ttl_locked(void) {
    (void)runtime_macro_dynamic_next_generation();
    runtime_macro_dynamic_state.ttl_deadline_ms = 0;
    runtime_macro_dynamic_state.ttl_work_generation = 0U;
    (void)k_work_cancel_delayable(&runtime_macro_dynamic_ttl_work);
}

static void runtime_macro_dynamic_cancel_staging_locked(void) {
    runtime_macro_dynamic_zeroize(runtime_macro_dynamic_state.staging,
                                  sizeof(runtime_macro_dynamic_state.staging));
    runtime_macro_dynamic_state.staging_expected_length = 0U;
    runtime_macro_dynamic_state.staging_received = 0U;
    runtime_macro_dynamic_state.staging_active = false;
    runtime_macro_dynamic_state.staging_ttl_seconds = 0U;
    runtime_macro_dynamic_state.staging_consume_on_accept = true;
}

static void runtime_macro_dynamic_clear_committed_locked(void) {
    runtime_macro_dynamic_zeroize(runtime_macro_dynamic_state.committed,
                                  sizeof(runtime_macro_dynamic_state.committed));
    runtime_macro_dynamic_state.committed_length = 0U;
    runtime_macro_dynamic_state.committed_valid = false;
    runtime_macro_dynamic_state.committed_consume_on_accept = true;
}

static void runtime_macro_dynamic_expire_locked(int64_t now_ms) {
    if (runtime_macro_dynamic_state.ttl_deadline_ms == 0 ||
        now_ms < runtime_macro_dynamic_state.ttl_deadline_ms ||
        runtime_macro_dynamic_state.ttl_work_generation == 0U ||
        runtime_macro_dynamic_state.ttl_work_generation !=
            runtime_macro_dynamic_state.ttl_generation) {
        return;
    }

    runtime_macro_dynamic_clear_committed_locked();
    runtime_macro_dynamic_cancel_staging_locked();
    runtime_macro_dynamic_cancel_ttl_locked();
}

static void runtime_macro_dynamic_schedule_ttl_locked(uint32_t ttl_seconds) {
    const int64_t ttl_ms = (int64_t)ttl_seconds * 1000;
    const uint32_t generation = runtime_macro_dynamic_next_generation();

    runtime_macro_dynamic_state.ttl_deadline_ms = k_uptime_get() + ttl_ms;
    runtime_macro_dynamic_state.ttl_work_generation = generation;
    (void)k_work_reschedule(&runtime_macro_dynamic_ttl_work, K_MSEC(ttl_ms));
}

static void runtime_macro_dynamic_ttl_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);

    if (runtime_macro_dynamic_state.ttl_deadline_ms != 0) {
        const int64_t now_ms = k_uptime_get();

        if (now_ms < runtime_macro_dynamic_state.ttl_deadline_ms &&
            runtime_macro_dynamic_state.ttl_work_generation ==
                runtime_macro_dynamic_state.ttl_generation) {
            const int64_t remaining_ms =
                runtime_macro_dynamic_state.ttl_deadline_ms - now_ms;
            (void)k_work_reschedule(&runtime_macro_dynamic_ttl_work,
                                    K_MSEC(remaining_ms));
        } else {
            runtime_macro_dynamic_expire_locked(now_ms);
        }
    }

    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

void zmk_runtime_macro_dynamic_reset(void) {
    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);

    (void)k_work_cancel_delayable(&runtime_macro_dynamic_ttl_work);
    runtime_macro_dynamic_zeroize(runtime_macro_dynamic_state.committed,
                                  sizeof(runtime_macro_dynamic_state.committed));
    runtime_macro_dynamic_zeroize(runtime_macro_dynamic_state.staging,
                                  sizeof(runtime_macro_dynamic_state.staging));
    runtime_macro_dynamic_state.committed_length = 0U;
    runtime_macro_dynamic_state.committed_valid = false;
    runtime_macro_dynamic_state.committed_consume_on_accept = true;
    runtime_macro_dynamic_state.staging_expected_length = 0U;
    runtime_macro_dynamic_state.staging_received = 0U;
    runtime_macro_dynamic_state.staging_active = false;
    runtime_macro_dynamic_state.staging_ttl_seconds = 0U;
    runtime_macro_dynamic_state.staging_consume_on_accept = true;
    runtime_macro_dynamic_state.ttl_deadline_ms = 0;
    runtime_macro_dynamic_state.ttl_generation = 0U;
    runtime_macro_dynamic_state.ttl_work_generation = 0U;

    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

int zmk_runtime_macro_dynamic_begin_with_options(size_t total_length,
                                                 uint32_t ttl_seconds,
                                                 bool consume_on_accept) {
    int err = 0;

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_locked(k_uptime_get());

    if (total_length == 0U ||
        total_length > ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN ||
        ttl_seconds < ZMK_RUNTIME_MACRO_DYNAMIC_MIN_TTL_SECONDS ||
        ttl_seconds > ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TTL_SECONDS) {
        runtime_macro_dynamic_cancel_staging_locked();
        err = -EINVAL;
        goto out;
    }

    runtime_macro_dynamic_cancel_staging_locked();
    runtime_macro_dynamic_state.staging_expected_length = total_length;
    runtime_macro_dynamic_state.staging_ttl_seconds = ttl_seconds;
    runtime_macro_dynamic_state.staging_consume_on_accept = consume_on_accept;
    runtime_macro_dynamic_state.staging_active = true;

out:
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
    return err;
}

int zmk_runtime_macro_dynamic_begin(size_t total_length, uint32_t ttl_seconds) {
    return zmk_runtime_macro_dynamic_begin_with_options(total_length, ttl_seconds,
                                                        true);
}

int zmk_runtime_macro_dynamic_append(size_t offset, const uint8_t *data,
                                     size_t length) {
    int err = 0;

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_locked(k_uptime_get());

    if (!runtime_macro_dynamic_state.staging_active) {
        err = -ENOENT;
        goto out;
    }

    if (data == NULL || length == 0U ||
        offset != runtime_macro_dynamic_state.staging_received ||
        offset > runtime_macro_dynamic_state.staging_expected_length ||
        length > runtime_macro_dynamic_state.staging_expected_length - offset) {
        runtime_macro_dynamic_cancel_staging_locked();
        err = -EINVAL;
        goto out;
    }

    for (size_t index = 0U; index < length; index++) {
        if (!runtime_macro_dynamic_byte_is_valid(data[index])) {
            runtime_macro_dynamic_cancel_staging_locked();
            err = -EINVAL;
            goto out;
        }
    }

    memcpy(runtime_macro_dynamic_state.staging + offset, data, length);
    runtime_macro_dynamic_state.staging_received += length;

    if (runtime_macro_dynamic_state.staging_received ==
        runtime_macro_dynamic_state.staging_expected_length) {
        const size_t committed_length =
            runtime_macro_dynamic_state.staging_expected_length;

        /* The mutex makes this replacement unobservable as a partial value. */
        memcpy(runtime_macro_dynamic_state.committed,
               runtime_macro_dynamic_state.staging, committed_length);
        runtime_macro_dynamic_zeroize(
            runtime_macro_dynamic_state.committed + committed_length,
            sizeof(runtime_macro_dynamic_state.committed) - committed_length);
        runtime_macro_dynamic_state.committed_length = committed_length;
        runtime_macro_dynamic_state.committed_valid = true;
        runtime_macro_dynamic_state.committed_consume_on_accept =
            runtime_macro_dynamic_state.staging_consume_on_accept;
        runtime_macro_dynamic_schedule_ttl_locked(
            runtime_macro_dynamic_state.staging_ttl_seconds);
        runtime_macro_dynamic_cancel_staging_locked();
    }

out:
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
    return err;
}

void zmk_runtime_macro_dynamic_cancel_staging(void) {
    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_locked(k_uptime_get());
    runtime_macro_dynamic_cancel_staging_locked();
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

void zmk_runtime_macro_dynamic_clear(void) {
    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_clear_committed_locked();
    runtime_macro_dynamic_cancel_staging_locked();
    runtime_macro_dynamic_cancel_ttl_locked();
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

void zmk_runtime_macro_dynamic_check_expiry(void) {
    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_locked(k_uptime_get());
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

int zmk_runtime_macro_dynamic_execute(void) {
    int err = 0;

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_locked(k_uptime_get());

    if (!runtime_macro_dynamic_state.committed_valid ||
        runtime_macro_dynamic_state.committed_length == 0U) {
        goto out;
    }

    err = zmk_runtime_macro_executor_start(
        runtime_macro_dynamic_state.committed,
        runtime_macro_dynamic_state.committed_length);
    if (err == 0 && runtime_macro_dynamic_state.committed_consume_on_accept) {
        /* The executor now owns an independent snapshot. */
        runtime_macro_dynamic_clear_committed_locked();
        runtime_macro_dynamic_cancel_ttl_locked();
    }

out:
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
    return err;
}

static int __attribute__((unused)) runtime_macro_dynamic_init(void) {
    zmk_runtime_macro_dynamic_reset();
    return 0;
}

SYS_INIT(runtime_macro_dynamic_init, APPLICATION,
         CONFIG_APPLICATION_INIT_PRIORITY);
