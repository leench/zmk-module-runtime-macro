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

_Static_assert(sizeof(runtime_macro_dynamic_state.slots[0].committed) ==
                   ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN,
               "dynamic slot buffer size must match the fixed maximum");
_Static_assert(sizeof(runtime_macro_dynamic_state.staging) ==
                   ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN,
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

static bool runtime_macro_dynamic_slot_is_valid(uint8_t slot) {
    return slot < (uint8_t)ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT;
}

uint8_t zmk_runtime_macro_dynamic_slot_count(void) {
    return (uint8_t)ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT;
}

/* ---------------------------------------------------------------- TTL work */

/*
 * Re-arm the single TTL work item for the earliest pending deadline. The work
 * item is cancelled when no slot has a deadline. Every call bumps
 * ttl_generation so a work item armed earlier can be told apart from the
 * current arming.
 */
static void runtime_macro_dynamic_rearm_ttl_locked(void) {
    int64_t earliest_deadline_ms = 0;

    for (uint8_t slot = 0U; slot < (uint8_t)ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT;
         slot++) {
        const struct zmk_runtime_macro_dynamic_slot *entry =
            &runtime_macro_dynamic_state.slots[slot];

        if (!entry->committed_valid || entry->ttl_deadline_ms == 0) {
            continue;
        }

        if (earliest_deadline_ms == 0 ||
            entry->ttl_deadline_ms < earliest_deadline_ms) {
            earliest_deadline_ms = entry->ttl_deadline_ms;
        }
    }

    runtime_macro_dynamic_state.ttl_generation++;
    runtime_macro_dynamic_state.ttl_work_generation =
        runtime_macro_dynamic_state.ttl_generation;

    if (earliest_deadline_ms == 0) {
        (void)k_work_cancel_delayable(&runtime_macro_dynamic_ttl_work);
        return;
    }

    const int64_t now_ms = k_uptime_get();
    const int64_t delay_ms =
        earliest_deadline_ms > now_ms ? earliest_deadline_ms - now_ms : 1;

    (void)k_work_reschedule(&runtime_macro_dynamic_ttl_work, K_MSEC(delay_ms));
}

static void runtime_macro_dynamic_clear_committed_locked(uint8_t slot) {
    struct zmk_runtime_macro_dynamic_slot *entry =
        &runtime_macro_dynamic_state.slots[slot];

    runtime_macro_dynamic_zeroize(entry->committed, sizeof(entry->committed));
    entry->committed_length = 0U;
    entry->committed_valid = false;
    entry->committed_consume_on_accept = true;
    entry->ttl_deadline_ms = 0;
    entry->ttl_generation++;
}

/*
 * Expire one slot: zeroize its committed text and drop its TTL. A staging
 * transaction targeting the same slot is cancelled, which preserves the v1
 * behaviour of expiring the single dynamic object.
 */
static void runtime_macro_dynamic_expire_slot_locked(uint8_t slot) {
    runtime_macro_dynamic_clear_committed_locked(slot);

    if (runtime_macro_dynamic_state.staging_active &&
        runtime_macro_dynamic_state.staging_slot == slot) {
        runtime_macro_dynamic_zeroize(runtime_macro_dynamic_state.staging,
                                      sizeof(runtime_macro_dynamic_state.staging));
        runtime_macro_dynamic_state.staging_slot = 0U;
        runtime_macro_dynamic_state.staging_expected_length = 0U;
        runtime_macro_dynamic_state.staging_received = 0U;
        runtime_macro_dynamic_state.staging_active = false;
        runtime_macro_dynamic_state.staging_ttl_seconds = 0U;
        runtime_macro_dynamic_state.staging_consume_on_accept = true;
    }
}

static void runtime_macro_dynamic_expire_due_locked(int64_t now_ms) {
    for (uint8_t slot = 0U; slot < (uint8_t)ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT;
         slot++) {
        const struct zmk_runtime_macro_dynamic_slot *entry =
            &runtime_macro_dynamic_state.slots[slot];

        if (!entry->committed_valid || entry->ttl_deadline_ms == 0 ||
            now_ms < entry->ttl_deadline_ms) {
            continue;
        }

        runtime_macro_dynamic_expire_slot_locked(slot);
    }
}

static void runtime_macro_dynamic_ttl_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);

    /*
     * A work item armed before a newer commit, clear, or lifecycle change is
     * stale: that change already re-armed the work item, so this run must not
     * expire anything. Expiry itself only ever touches slots whose current
     * deadline has really passed.
     */
    if (runtime_macro_dynamic_state.ttl_work_generation ==
        runtime_macro_dynamic_state.ttl_generation) {
        runtime_macro_dynamic_expire_due_locked(k_uptime_get());
    }

    runtime_macro_dynamic_rearm_ttl_locked();

    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

/* --------------------------------------------------------------- staging */

static void runtime_macro_dynamic_cancel_staging_locked(void) {
    runtime_macro_dynamic_zeroize(runtime_macro_dynamic_state.staging,
                                  sizeof(runtime_macro_dynamic_state.staging));
    runtime_macro_dynamic_state.staging_slot = 0U;
    runtime_macro_dynamic_state.staging_expected_length = 0U;
    runtime_macro_dynamic_state.staging_received = 0U;
    runtime_macro_dynamic_state.staging_active = false;
    runtime_macro_dynamic_state.staging_ttl_seconds = 0U;
    runtime_macro_dynamic_state.staging_consume_on_accept = true;
}

static int runtime_macro_dynamic_begin_locked(uint8_t slot, size_t total_length,
                                              size_t max_length,
                                              uint32_t ttl_seconds,
                                              bool consume_on_accept) {
    if (!runtime_macro_dynamic_slot_is_valid(slot) || total_length == 0U ||
        total_length > max_length ||
        ttl_seconds < ZMK_RUNTIME_MACRO_DYNAMIC_MIN_TTL_SECONDS ||
        ttl_seconds > ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TTL_SECONDS) {
        runtime_macro_dynamic_cancel_staging_locked();
        return -EINVAL;
    }

    runtime_macro_dynamic_cancel_staging_locked();
    runtime_macro_dynamic_state.staging_slot = slot;
    runtime_macro_dynamic_state.staging_expected_length = total_length;
    runtime_macro_dynamic_state.staging_ttl_seconds = ttl_seconds;
    runtime_macro_dynamic_state.staging_consume_on_accept = consume_on_accept;
    runtime_macro_dynamic_state.staging_active = true;

    return 0;
}

static void runtime_macro_dynamic_commit_locked(void) {
    const uint8_t slot = runtime_macro_dynamic_state.staging_slot;
    struct zmk_runtime_macro_dynamic_slot *entry =
        &runtime_macro_dynamic_state.slots[slot];
    const size_t committed_length =
        runtime_macro_dynamic_state.staging_expected_length;

    /* The mutex makes this replacement unobservable as a partial value. */
    memcpy(entry->committed, runtime_macro_dynamic_state.staging,
           committed_length);
    runtime_macro_dynamic_zeroize(entry->committed + committed_length,
                                  sizeof(entry->committed) - committed_length);
    entry->committed_length = (uint16_t)committed_length;
    entry->committed_valid = true;
    entry->committed_consume_on_accept =
        runtime_macro_dynamic_state.staging_consume_on_accept;
    entry->ttl_deadline_ms =
        k_uptime_get() +
        (int64_t)runtime_macro_dynamic_state.staging_ttl_seconds * 1000;
    entry->ttl_generation++;

    runtime_macro_dynamic_cancel_staging_locked();
    runtime_macro_dynamic_rearm_ttl_locked();
}

static int runtime_macro_dynamic_append_locked(uint8_t slot, size_t offset,
                                               const uint8_t *data,
                                               size_t length) {
    if (!runtime_macro_dynamic_state.staging_active) {
        return -ENOENT;
    }

    if (!runtime_macro_dynamic_slot_is_valid(slot) ||
        slot != runtime_macro_dynamic_state.staging_slot) {
        runtime_macro_dynamic_cancel_staging_locked();
        return -EINVAL;
    }

    if (data == NULL || length == 0U ||
        offset != runtime_macro_dynamic_state.staging_received ||
        offset > runtime_macro_dynamic_state.staging_expected_length ||
        length > runtime_macro_dynamic_state.staging_expected_length - offset) {
        runtime_macro_dynamic_cancel_staging_locked();
        return -EINVAL;
    }

    for (size_t index = 0U; index < length; index++) {
        if (!runtime_macro_dynamic_byte_is_valid(data[index])) {
            runtime_macro_dynamic_cancel_staging_locked();
            return -EINVAL;
        }
    }

    memcpy(runtime_macro_dynamic_state.staging + offset, data, length);
    runtime_macro_dynamic_state.staging_received += length;

    if (runtime_macro_dynamic_state.staging_received ==
        runtime_macro_dynamic_state.staging_expected_length) {
        runtime_macro_dynamic_commit_locked();
    }

    return 0;
}

/* ----------------------------------------------------------------- public */

void zmk_runtime_macro_dynamic_reset(void) {
    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);

    (void)k_work_cancel_delayable(&runtime_macro_dynamic_ttl_work);

    for (uint8_t slot = 0U; slot < (uint8_t)ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT;
         slot++) {
        struct zmk_runtime_macro_dynamic_slot *entry =
            &runtime_macro_dynamic_state.slots[slot];

        runtime_macro_dynamic_zeroize(entry->committed, sizeof(entry->committed));
        entry->committed_length = 0U;
        entry->committed_valid = false;
        entry->committed_consume_on_accept = true;
        entry->ttl_deadline_ms = 0;
        entry->ttl_generation = 0U;
    }

    runtime_macro_dynamic_cancel_staging_locked();
    runtime_macro_dynamic_state.ttl_generation = 0U;
    runtime_macro_dynamic_state.ttl_work_generation = 0U;

    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

int zmk_runtime_macro_dynamic_begin_slot(uint8_t slot, size_t total_length,
                                         uint32_t ttl_seconds,
                                         bool consume_on_accept) {
    int err;

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_due_locked(k_uptime_get());
    err = runtime_macro_dynamic_begin_locked(
        slot, total_length, ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN,
        ttl_seconds, consume_on_accept);
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);

    return err;
}

int zmk_runtime_macro_dynamic_append_slot(uint8_t slot, size_t offset,
                                          const uint8_t *data, size_t length) {
    int err;

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_due_locked(k_uptime_get());
    err = runtime_macro_dynamic_append_locked(slot, offset, data, length);
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);

    return err;
}

void zmk_runtime_macro_dynamic_cancel_staging(void) {
    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_due_locked(k_uptime_get());
    runtime_macro_dynamic_cancel_staging_locked();
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

void zmk_runtime_macro_dynamic_clear_slot(uint8_t slot) {
    if (!runtime_macro_dynamic_slot_is_valid(slot)) {
        return;
    }

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_slot_locked(slot);
    runtime_macro_dynamic_rearm_ttl_locked();
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

void zmk_runtime_macro_dynamic_clear_all(void) {
    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);

    for (uint8_t slot = 0U; slot < (uint8_t)ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT;
         slot++) {
        runtime_macro_dynamic_clear_committed_locked(slot);
    }

    runtime_macro_dynamic_cancel_staging_locked();
    runtime_macro_dynamic_rearm_ttl_locked();
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

void zmk_runtime_macro_dynamic_check_expiry(void) {
    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_due_locked(k_uptime_get());
    runtime_macro_dynamic_rearm_ttl_locked();
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
}

int zmk_runtime_macro_dynamic_execute_slot(uint8_t slot) {
    int err = 0;

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_due_locked(k_uptime_get());

    if (!runtime_macro_dynamic_slot_is_valid(slot)) {
        err = -EINVAL;
        goto out;
    }

    struct zmk_runtime_macro_dynamic_slot *entry =
        &runtime_macro_dynamic_state.slots[slot];

    if (!entry->committed_valid || entry->committed_length == 0U) {
        goto out;
    }

    /*
     * The shared executor snapshot holds the full per-slot maximum since D2,
     * so a committed slot is always executable without truncation.
     */
    err = zmk_runtime_macro_executor_start(entry->committed,
                                           entry->committed_length);
    if (err == 0 && entry->committed_consume_on_accept) {
        /* The executor now owns an independent snapshot. */
        runtime_macro_dynamic_clear_committed_locked(slot);
        runtime_macro_dynamic_rearm_ttl_locked();
    }

out:
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);
    return err;
}

int zmk_runtime_macro_dynamic_begin_with_options(size_t total_length,
                                                 uint32_t ttl_seconds,
                                                 bool consume_on_accept) {
    int err;

    (void)k_mutex_lock(&runtime_macro_dynamic_mutex, K_FOREVER);
    runtime_macro_dynamic_expire_due_locked(k_uptime_get());
    err = runtime_macro_dynamic_begin_locked(
        ZMK_RUNTIME_MACRO_DYNAMIC_LEGACY_SLOT, total_length,
        ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN, ttl_seconds,
        consume_on_accept);
    (void)k_mutex_unlock(&runtime_macro_dynamic_mutex);

    return err;
}

int zmk_runtime_macro_dynamic_begin(size_t total_length, uint32_t ttl_seconds) {
    return zmk_runtime_macro_dynamic_begin_with_options(total_length, ttl_seconds,
                                                        true);
}

int zmk_runtime_macro_dynamic_append(size_t offset, const uint8_t *data,
                                     size_t length) {
    return zmk_runtime_macro_dynamic_append_slot(
        ZMK_RUNTIME_MACRO_DYNAMIC_LEGACY_SLOT, offset, data, length);
}

int zmk_runtime_macro_dynamic_execute(void) {
    return zmk_runtime_macro_dynamic_execute_slot(
        ZMK_RUNTIME_MACRO_DYNAMIC_LEGACY_SLOT);
}

void zmk_runtime_macro_dynamic_clear(void) {
    zmk_runtime_macro_dynamic_clear_all();
}

static int __attribute__((unused)) runtime_macro_dynamic_init(void) {
    zmk_runtime_macro_dynamic_reset();
    return 0;
}

SYS_INIT(runtime_macro_dynamic_init, APPLICATION,
         CONFIG_APPLICATION_INIT_PRIORITY);
