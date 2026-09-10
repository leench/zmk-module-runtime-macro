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
#include <zephyr/settings/settings.h>

#include "../../src/runtime_macro_ascii.c"
#include "../../src/runtime_macro_dynamic.c"

static int executor_start_result;
static unsigned int executor_start_calls;
static size_t executor_start_lengths[16];
static unsigned int executor_start_length_count;

int zmk_runtime_macro_executor_start(const uint8_t *text, size_t length) {
    (void)text;
    executor_start_calls++;
    if (executor_start_length_count <
        sizeof(executor_start_lengths) / sizeof(executor_start_lengths[0])) {
        executor_start_lengths[executor_start_length_count++] = length;
    }
    return executor_start_result;
}

/*
 * The RAM-only dynamic store must never reach the persistence or static slot
 * paths. The stubs record every call so the test can assert a count of zero.
 */
static unsigned int settings_save_calls;
static unsigned int settings_delete_calls;
static unsigned int static_slot_set_calls;
static unsigned int static_slot_clear_calls;

int settings_save_one(const char *name, const void *value, size_t val_len) {
    (void)name;
    (void)value;
    (void)val_len;
    settings_save_calls++;
    return 0;
}

int settings_delete(const char *name) {
    (void)name;
    settings_delete_calls++;
    return 0;
}

int zmk_runtime_macro_slot_set(uint8_t slot, const char *text, size_t length) {
    (void)slot;
    (void)text;
    (void)length;
    static_slot_set_calls++;
    return 0;
}

int zmk_runtime_macro_slot_clear(uint8_t slot) {
    (void)slot;
    static_slot_clear_calls++;
    return 0;
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

#define SLOT_COUNT ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT
#define SLOT_MAX ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN

#define slot_state(index) (runtime_macro_dynamic_state.slots[(index)])

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

static void reset_store(void) {
    zmk_runtime_macro_dynamic_reset();
    host_uptime = 0;
    executor_start_result = 0;
    executor_start_calls = 0U;
    executor_start_length_count = 0U;
}

static void assert_slot_text(uint8_t slot, const uint8_t *expected,
                             size_t length) {
    assert(slot_state(slot).committed_valid);
    assert(slot_state(slot).committed_length == length);
    assert(memcmp(slot_state(slot).committed, expected, length) == 0);
    assert(all_zero(slot_state(slot).committed + length,
                    sizeof(slot_state(slot).committed) - length));
}

static void assert_slot_empty(uint8_t slot) {
    assert(!slot_state(slot).committed_valid);
    assert(slot_state(slot).committed_length == 0U);
    assert(slot_state(slot).ttl_deadline_ms == 0);
    assert(slot_state(slot).committed_consume_on_accept);
    assert(all_zero(slot_state(slot).committed,
                    sizeof(slot_state(slot).committed)));
}

static void assert_all_slots_empty(void) {
    for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
        assert_slot_empty(slot);
    }
}

static void assert_staging_empty(void) {
    assert(!runtime_macro_dynamic_state.staging_active);
    assert(runtime_macro_dynamic_state.staging_slot == 0U);
    assert(runtime_macro_dynamic_state.staging_expected_length == 0U);
    assert(runtime_macro_dynamic_state.staging_received == 0U);
    assert(runtime_macro_dynamic_state.staging_ttl_seconds == 0U);
    assert(runtime_macro_dynamic_state.staging_consume_on_accept);
    assert(all_zero(runtime_macro_dynamic_state.staging,
                    sizeof(runtime_macro_dynamic_state.staging)));
}

static void commit_slot_with_policy(uint8_t slot, const uint8_t *text,
                                    size_t length, uint32_t ttl_seconds,
                                    bool consume_on_accept) {
    assert(zmk_runtime_macro_dynamic_begin_slot(slot, length, ttl_seconds,
                                                consume_on_accept) == 0);
    assert(zmk_runtime_macro_dynamic_append_slot(slot, 0U, text, length) == 0);
    assert_slot_text(slot, text, length);
    assert_staging_empty();
}

static void commit_slot(uint8_t slot, const uint8_t *text, size_t length,
                        uint32_t ttl_seconds) {
    commit_slot_with_policy(slot, text, length, ttl_seconds, true);
}

static void test_reset_and_slot_layout(void) {
    uint8_t filler[SLOT_MAX];

    memset(filler, 'A', sizeof(filler));
    assert(SLOT_COUNT == 8U);
    assert(SLOT_MAX == 512U);
    assert(zmk_runtime_macro_dynamic_slot_count() == 8U);
    assert(sizeof(slot_state(0).committed) == 512U);
    assert(sizeof(runtime_macro_dynamic_state.staging) == 512U);

    reset_store();
    assert_all_slots_empty();
    assert_staging_empty();
    assert(!runtime_macro_dynamic_ttl_work.scheduled);

    /* Every slot starts clean after a reset, including a previously used one. */
    for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
        commit_slot(slot, filler, sizeof(filler),
                    ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS);
    }

    zmk_runtime_macro_dynamic_reset();
    assert_all_slots_empty();
    assert_staging_empty();
    assert(!runtime_macro_dynamic_ttl_work.scheduled);
    assert(runtime_macro_dynamic_state.ttl_generation == 0U);
    assert(runtime_macro_dynamic_state.ttl_work_generation == 0U);
}

static void test_slot_indexing_and_bounds(void) {
    const uint8_t text[] = "slot text";
    uint8_t per_slot[SLOT_COUNT][sizeof(text) - 1U];

    reset_store();

    /* Every slot 0..7 is independently addressable. */
    for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
        memset(per_slot[slot], (int)('a' + slot), sizeof(per_slot[slot]));
        commit_slot(slot, per_slot[slot], sizeof(per_slot[slot]),
                    ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS);
    }
    for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
        assert_slot_text(slot, per_slot[slot], sizeof(per_slot[slot]));
    }

    /* Out-of-range slots are rejected without touching any committed text. */
    assert(zmk_runtime_macro_dynamic_begin_slot((uint8_t)SLOT_COUNT, 4U, 300U,
                                                true) == -EINVAL);
    assert(zmk_runtime_macro_dynamic_begin_slot(0xffU, 4U, 300U, true) ==
           -EINVAL);
    assert_staging_empty();
    assert(zmk_runtime_macro_dynamic_append_slot((uint8_t)SLOT_COUNT, 0U, text,
                                                 4U) == -ENOENT);
    assert(zmk_runtime_macro_dynamic_execute_slot((uint8_t)SLOT_COUNT) ==
           -EINVAL);
    assert(zmk_runtime_macro_dynamic_execute_slot(0xffU) == -EINVAL);
    zmk_runtime_macro_dynamic_clear_slot((uint8_t)SLOT_COUNT);
    zmk_runtime_macro_dynamic_clear_slot(0xffU);

    for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
        assert_slot_text(slot, per_slot[slot], sizeof(per_slot[slot]));
    }

    /* A mismatched DATA slot cancels staging but keeps every committed slot. */
    assert(zmk_runtime_macro_dynamic_begin_slot(3U, 4U, 300U, true) == 0);
    assert(zmk_runtime_macro_dynamic_append_slot(4U, 0U, text, 2U) == -EINVAL);
    assert_staging_empty();
    for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
        assert_slot_text(slot, per_slot[slot], sizeof(per_slot[slot]));
    }
}

static void test_length_boundaries(void) {
    const size_t lengths[] = {1U, 22U, 23U, 256U, 511U, 512U};
    uint8_t text[SLOT_MAX];

    memset(text, 'L', sizeof(text));

    for (size_t index = 0U; index < sizeof(lengths) / sizeof(lengths[0]);
         index++) {
        const uint8_t slot = (uint8_t)index;

        reset_store();
        assert(zmk_runtime_macro_dynamic_begin_slot(
                   slot, lengths[index],
                   ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS, true) == 0);
        assert(runtime_macro_dynamic_state.staging_slot == slot);
        assert(zmk_runtime_macro_dynamic_append_slot(slot, 0U, text,
                                                     lengths[index]) == 0);
        assert_slot_text(slot, text, lengths[index]);
        assert_staging_empty();
    }

    /* The per-slot maximum is exactly 512 bytes. */
    reset_store();
    assert(zmk_runtime_macro_dynamic_begin_slot(7U, 513U, 300U, true) ==
           -EINVAL);
    assert_staging_empty();
    assert(zmk_runtime_macro_dynamic_begin_slot(7U, 0U, 300U, true) == -EINVAL);
    assert_staging_empty();
    assert(zmk_runtime_macro_dynamic_begin_slot(7U, 1U, 0U, true) == -EINVAL);
    assert(zmk_runtime_macro_dynamic_begin_slot(
               7U, 1U, ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TTL_SECONDS + 1U, true) ==
           -EINVAL);
    assert_staging_empty();
    assert_all_slots_empty();

    /* A 512-byte upload spans 24 chunks of at most 22 bytes. */
    reset_store();
    assert(zmk_runtime_macro_dynamic_begin_slot(7U, 512U, 300U, true) == 0);
    for (uint16_t offset = 0U; offset < 512U;) {
        const uint16_t remaining = (uint16_t)(512U - offset);
        const uint16_t chunk = remaining > 22U ? 22U : remaining;

        assert(zmk_runtime_macro_dynamic_append_slot(7U, offset, text, chunk) ==
               0);
        offset = (uint16_t)(offset + chunk);
    }
    assert_slot_text(7U, text, 512U);
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

    reset_store();
    commit_slot(2U, allowed, allowed_length,
                ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS);

    const uint8_t invalid[] = {0x00U, 0x01U, 0x07U, 0x0bU, 0x0cU,
                               0x1fU, 0x7fU, 0x80U, 0xffU};

    for (size_t index = 0U; index < sizeof(invalid); index++) {
        assert(zmk_runtime_macro_dynamic_begin_slot(
                   2U, 1U, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS,
                   true) == 0);
        assert(zmk_runtime_macro_dynamic_append_slot(2U, 0U, &invalid[index],
                                                     1U) == -EINVAL);
        assert_slot_text(2U, allowed, allowed_length);
        assert_staging_empty();
    }
}

static void test_shared_staging_and_atomic_commit(void) {
    const uint8_t old_text[] = "old";
    const uint8_t first[] = "new";
    const uint8_t final[] = "12";
    const uint8_t too_long[] = "new123";
    const uint8_t other_text[] = "other slot";

    reset_store();
    commit_slot(0U, old_text, sizeof(old_text) - 1U, 300U);
    commit_slot(1U, other_text, sizeof(other_text) - 1U, 300U);

    assert(zmk_runtime_macro_dynamic_begin_slot(0U, 5U, 300U, true) == 0);
    assert(zmk_runtime_macro_dynamic_append_slot(0U, 0U, first,
                                                 sizeof(first) - 1U) == 0);
    assert_slot_text(0U, old_text, sizeof(old_text) - 1U);
    assert(runtime_macro_dynamic_state.staging_received == 3U);

    /* A new BEGIN for another slot replaces the shared staging transaction
     * without touching any committed text. */
    assert(zmk_runtime_macro_dynamic_begin_slot(2U, 2U, 300U, true) == 0);
    assert(runtime_macro_dynamic_state.staging_slot == 2U);
    assert(runtime_macro_dynamic_state.staging_received == 0U);
    assert(all_zero(runtime_macro_dynamic_state.staging,
                    sizeof(runtime_macro_dynamic_state.staging)));
    assert_slot_text(0U, old_text, sizeof(old_text) - 1U);
    assert_slot_text(1U, other_text, sizeof(other_text) - 1U);

    assert(zmk_runtime_macro_dynamic_append_slot(2U, 0U, final,
                                                 sizeof(final) - 1U) == 0);
    assert_slot_text(2U, (const uint8_t *)"12", 2U);
    assert_slot_text(0U, old_text, sizeof(old_text) - 1U);
    assert_slot_text(1U, other_text, sizeof(other_text) - 1U);
    assert_staging_empty();

    /* Bad offset and bad length cancel staging only. */
    assert(zmk_runtime_macro_dynamic_begin_slot(0U, 4U, 300U, true) == 0);
    assert(zmk_runtime_macro_dynamic_append_slot(0U, 1U, first, 1U) == -EINVAL);
    assert_staging_empty();
    assert_slot_text(0U, old_text, sizeof(old_text) - 1U);

    assert(zmk_runtime_macro_dynamic_begin_slot(0U, 4U, 300U, true) == 0);
    assert(zmk_runtime_macro_dynamic_append_slot(0U, 0U, too_long,
                                                 sizeof(too_long) - 1U) ==
           -EINVAL);
    assert_staging_empty();
    assert_slot_text(0U, old_text, sizeof(old_text) - 1U);

    assert(zmk_runtime_macro_dynamic_begin_slot(0U, 4U, 300U, true) == 0);
    assert(zmk_runtime_macro_dynamic_append_slot(0U, 0U, NULL, 1U) == -EINVAL);
    assert_staging_empty();
    assert_slot_text(0U, old_text, sizeof(old_text) - 1U);

    assert(zmk_runtime_macro_dynamic_begin_slot(0U, 4U, 300U, true) == 0);
    assert(zmk_runtime_macro_dynamic_append_slot(0U, 0U, first, 0U) == -EINVAL);
    assert_staging_empty();
    assert_slot_text(0U, old_text, sizeof(old_text) - 1U);

    /* Appending without an active transaction reports -ENOENT and changes
     * nothing. */
    assert(zmk_runtime_macro_dynamic_append_slot(0U, 0U, first,
                                                 sizeof(first) - 1U) ==
           -ENOENT);
    assert_slot_text(0U, old_text, sizeof(old_text) - 1U);
    assert_slot_text(1U, other_text, sizeof(other_text) - 1U);
    assert_slot_text(2U, (const uint8_t *)"12", 2U);
}

static void test_legacy_slot_zero_entry_points(void) {
    const uint8_t text[] = "legacy";
    const uint8_t other[] = "keep me";
    uint8_t maximum[ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN];

    memset(maximum, 'M', sizeof(maximum));

    reset_store();

    /* The frozen v1 boundary is 256 bytes on the legacy path. */
    assert(zmk_runtime_macro_dynamic_begin(
               ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN + 1U, 300U) == -EINVAL);
    assert_staging_empty();
    assert(zmk_runtime_macro_dynamic_begin(0U, 300U) == -EINVAL);
    assert_staging_empty();

    assert(zmk_runtime_macro_dynamic_begin(
               ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN, 300U) == 0);
    assert(runtime_macro_dynamic_state.staging_slot == 0U);
    assert(zmk_runtime_macro_dynamic_append(0U, maximum, sizeof(maximum)) == 0);
    assert_slot_text(0U, maximum, sizeof(maximum));

    /* Legacy entry points address slot 0 only. */
    commit_slot(5U, other, sizeof(other) - 1U, 300U);
    assert(zmk_runtime_macro_dynamic_begin(sizeof(text) - 1U, 300U) == 0);
    assert(runtime_macro_dynamic_state.staging_slot == 0U);
    assert(zmk_runtime_macro_dynamic_append(0U, text, sizeof(text) - 1U) == 0);
    assert_slot_text(0U, text, sizeof(text) - 1U);
    assert_slot_text(5U, other, sizeof(other) - 1U);

    /* Legacy execution consumes slot 0 and leaves other slots alone. */
    executor_start_result = 0;
    executor_start_calls = 0U;
    assert(zmk_runtime_macro_dynamic_execute() == 0);
    assert(executor_start_calls == 1U);
    assert_slot_empty(0U);
    assert_slot_text(5U, other, sizeof(other) - 1U);

    /* Legacy clear keeps v1 whole-state semantics. */
    commit_slot(1U, text, sizeof(text) - 1U, 300U);
    assert(zmk_runtime_macro_dynamic_begin_slot(6U, 3U, 300U, true) == 0);
    zmk_runtime_macro_dynamic_clear();
    assert_all_slots_empty();
    assert_staging_empty();
    assert(!runtime_macro_dynamic_ttl_work.scheduled);

    /*
     * A legacy transaction must never continue another slot's staging: the
     * legacy WRITE path cancels it and STARTs slot 0 instead.
     */
    commit_slot(3U, other, sizeof(other) - 1U, 300U);
    assert(zmk_runtime_macro_dynamic_begin_slot(5U, 4U, 300U, true) == 0);
    assert(zmk_runtime_macro_dynamic_append_slot(5U, 0U, text, 2U) == 0);
    assert(zmk_runtime_macro_dynamic_append(2U, text, 2U) == -EINVAL);
    assert_staging_empty();
    assert_slot_text(3U, other, sizeof(other) - 1U);

    assert(zmk_runtime_macro_dynamic_begin_slot(5U, 4U, 300U, true) == 0);
    assert(zmk_runtime_macro_dynamic_begin(sizeof(text) - 1U, 300U) == 0);
    assert(runtime_macro_dynamic_state.staging_slot == 0U);
    assert(zmk_runtime_macro_dynamic_append(0U, text, sizeof(text) - 1U) == 0);
    assert_slot_text(0U, text, sizeof(text) - 1U);
    assert_slot_text(3U, other, sizeof(other) - 1U);
}

static void test_execution_policy_and_oversize(void) {
    const uint8_t text[] = "repeatable";
    uint8_t oversized[ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN + 4U];

    memset(oversized, 'O', sizeof(oversized));

    reset_store();
    executor_start_result = 0;
    executor_start_calls = 0U;
    commit_slot(4U, text, sizeof(text) - 1U, 300U);
    assert(slot_state(4U).committed_consume_on_accept);
    assert(zmk_runtime_macro_dynamic_execute_slot(4U) == 0);
    assert(executor_start_calls == 1U);
    assert(executor_start_lengths[0] == sizeof(text) - 1U);
    assert_slot_empty(4U);

    reset_store();
    commit_slot_with_policy(4U, text, sizeof(text) - 1U, 300U, false);
    assert(!slot_state(4U).committed_consume_on_accept);
    assert(zmk_runtime_macro_dynamic_execute_slot(4U) == 0);
    assert(executor_start_calls == 1U);
    assert_slot_text(4U, text, sizeof(text) - 1U);

    executor_start_result = -EBUSY;
    assert(zmk_runtime_macro_dynamic_execute_slot(4U) == -EBUSY);
    assert(executor_start_calls == 2U);
    assert_slot_text(4U, text, sizeof(text) - 1U);
    executor_start_result = 0;

    /* Empty slots are harmless. */
    assert(zmk_runtime_macro_dynamic_execute_slot(0U) == 0);
    assert(executor_start_calls == 2U);

    /*
     * Text longer than the current executor capacity is refused instead of
     * being truncated, and stays available in the slot.
     */
    reset_store();
    commit_slot(4U, oversized, sizeof(oversized), 300U);
    assert(zmk_runtime_macro_dynamic_execute_slot(4U) == -EINVAL);
    assert(executor_start_calls == 0U);
    assert_slot_text(4U, oversized, sizeof(oversized));
    assert(zmk_runtime_macro_dynamic_execute() == 0);

    reset_store();
    commit_slot(0U, oversized, sizeof(oversized), 300U);
    assert(zmk_runtime_macro_dynamic_execute() == -EINVAL);
    assert(executor_start_calls == 0U);
    assert_slot_text(0U, oversized, sizeof(oversized));
}

static void test_clear_slot_and_clear_all(void) {
    const uint8_t text[] = "clear me";

    reset_store();

    for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
        commit_slot(slot, text, sizeof(text) - 1U, 300U);
    }

    /* Clearing one slot keeps every other slot and its deadline. */
    const int64_t kept_deadline = slot_state(1U).ttl_deadline_ms;

    zmk_runtime_macro_dynamic_clear_slot(0U);
    assert_slot_empty(0U);
    assert(slot_state(1U).committed_valid);
    assert(slot_state(1U).ttl_deadline_ms == kept_deadline);
    assert(runtime_macro_dynamic_ttl_work.scheduled);

    /* Clearing is idempotent. */
    zmk_runtime_macro_dynamic_clear_slot(0U);
    assert_slot_empty(0U);
    assert(slot_state(1U).committed_valid);

    /* Staging is only cancelled when it targets the cleared slot. */
    assert(zmk_runtime_macro_dynamic_begin_slot(5U, 3U, 300U, true) == 0);
    zmk_runtime_macro_dynamic_clear_slot(2U);
    assert(runtime_macro_dynamic_state.staging_active);
    assert(runtime_macro_dynamic_state.staging_slot == 5U);
    zmk_runtime_macro_dynamic_clear_slot(5U);
    assert_staging_empty();
    assert_slot_empty(5U);

    /* clear_all clears every slot, staging, and the pending TTL work. */
    assert(zmk_runtime_macro_dynamic_begin_slot(6U, 3U, 300U, true) == 0);
    zmk_runtime_macro_dynamic_clear_all();
    assert_all_slots_empty();
    assert_staging_empty();
    assert(!runtime_macro_dynamic_ttl_work.scheduled);
    zmk_runtime_macro_dynamic_clear_all();
    assert_all_slots_empty();
    assert_staging_empty();
    assert(!runtime_macro_dynamic_ttl_work.scheduled);
}

static void test_per_slot_ttl_and_earliest_deadline(void) {
    const uint8_t long_ttl[] = "long";
    const uint8_t short_ttl[] = "short";
    const uint8_t third[] = "third";

    reset_store();
    host_uptime = 1000;

    commit_slot(1U, long_ttl, sizeof(long_ttl) - 1U, 10U);
    assert(slot_state(1U).ttl_deadline_ms == 11000);
    assert(runtime_macro_dynamic_ttl_work.delay == 10000);

    /* The single work item always follows the earliest deadline. */
    commit_slot(3U, short_ttl, sizeof(short_ttl) - 1U, 3U);
    assert(slot_state(3U).ttl_deadline_ms == 4000);
    assert(runtime_macro_dynamic_ttl_work.scheduled);
    assert(runtime_macro_dynamic_ttl_work.delay == 3000);

    const uint32_t long_generation = slot_state(1U).ttl_generation;

    host_uptime = 3999;
    run_ttl_work();
    assert_slot_text(1U, long_ttl, sizeof(long_ttl) - 1U);
    assert_slot_text(3U, short_ttl, sizeof(short_ttl) - 1U);
    assert(runtime_macro_dynamic_ttl_work.delay == 1);

    host_uptime = 4000;
    run_ttl_work();
    assert_slot_text(1U, long_ttl, sizeof(long_ttl) - 1U);
    assert_slot_empty(3U);
    assert(slot_state(1U).ttl_generation == long_generation);
    assert(runtime_macro_dynamic_ttl_work.scheduled);
    assert(runtime_macro_dynamic_ttl_work.delay == 7000);

    /* An expiry in another slot must not disturb a newer commit. */
    commit_slot(5U, third, sizeof(third) - 1U, 1U);
    assert(runtime_macro_dynamic_ttl_work.delay == 1000);

    host_uptime = 5000;
    run_ttl_work();
    assert_slot_empty(5U);
    assert_slot_text(1U, long_ttl, sizeof(long_ttl) - 1U);
    assert(runtime_macro_dynamic_ttl_work.delay == 6000);

    host_uptime = 11000;
    run_ttl_work();
    assert_slot_empty(1U);
    assert_all_slots_empty();
    assert(!runtime_macro_dynamic_ttl_work.scheduled);

    /* check_expiry applies the same rule without the work item. */
    reset_store();
    host_uptime = 200;
    commit_slot(2U, third, sizeof(third) - 1U, 1U);
    host_uptime = 1200;
    zmk_runtime_macro_dynamic_check_expiry();
    assert_slot_empty(2U);
    assert(!runtime_macro_dynamic_ttl_work.scheduled);
}

static void test_ttl_stale_generation(void) {
    const uint8_t first[] = "first";
    const uint8_t second[] = "second";

    reset_store();
    host_uptime = 0;
    commit_slot(0U, first, sizeof(first) - 1U, 300U);
    const uint32_t old_work_generation =
        runtime_macro_dynamic_state.ttl_work_generation;
    const uint32_t old_slot_generation = slot_state(0U).ttl_generation;
    assert(runtime_macro_dynamic_ttl_work.delay == 300000);

    /* Replace slot 0 with a much shorter TTL before the old work fires. */
    host_uptime = 1000;
    commit_slot(0U, second, sizeof(second) - 1U, 1U);
    assert(slot_state(0U).ttl_generation != old_slot_generation);
    assert(runtime_macro_dynamic_state.ttl_work_generation !=
           old_work_generation);
    assert(runtime_macro_dynamic_ttl_work.delay == 1000);

    /*
     * Simulate the stale work item (armed for the replaced value) firing after
     * the replacement: it must not expire the newer commit.
     */
    runtime_macro_dynamic_state.ttl_work_generation = old_work_generation;
    host_uptime = 2000;
    run_ttl_work();
    assert_slot_text(0U, second, sizeof(second) - 1U);
    assert(runtime_macro_dynamic_ttl_work.scheduled);

    /* The re-armed work item then expires the newer commit at its deadline. */
    run_ttl_work();
    assert_slot_empty(0U);
    assert(!runtime_macro_dynamic_ttl_work.scheduled);

    /* A second stale run after the expiry is harmless. */
    runtime_macro_dynamic_state.ttl_work_generation = old_work_generation;
    run_ttl_work();
    assert_slot_empty(0U);

    /* A stale run must not touch another slot either. */
    commit_slot(4U, first, sizeof(first) - 1U, 5U);
    const uint32_t replacement_generation =
        runtime_macro_dynamic_state.ttl_work_generation;
    runtime_macro_dynamic_state.ttl_work_generation = old_work_generation;
    host_uptime = 3000;
    run_ttl_work();
    assert_slot_text(4U, first, sizeof(first) - 1U);
    assert(runtime_macro_dynamic_state.ttl_work_generation >
           replacement_generation);
    assert(runtime_macro_dynamic_state.ttl_work_generation ==
           runtime_macro_dynamic_state.ttl_generation);
    assert(runtime_macro_dynamic_ttl_work.scheduled);
    assert(runtime_macro_dynamic_ttl_work.delay == 4000);

    host_uptime = 7000;
    run_ttl_work();
    assert_slot_empty(4U);
    assert(!runtime_macro_dynamic_ttl_work.scheduled);
}

static void test_commit_replaces_ttl_and_zeroizes(void) {
    const uint8_t long_text[] = "0123456789";
    const uint8_t short_text[] = "xy";
    uint32_t generation;

    reset_store();
    host_uptime = 500;

    commit_slot(3U, long_text, sizeof(long_text) - 1U, 30U);
    generation = slot_state(3U).ttl_generation;
    assert(slot_state(3U).ttl_deadline_ms == 30500);

    host_uptime = 1000;
    commit_slot(3U, short_text, sizeof(short_text) - 1U, 20U);
    assert(slot_state(3U).committed_length == 2U);
    assert(slot_state(3U).ttl_deadline_ms == 21000);
    assert(slot_state(3U).ttl_generation != generation);
    assert(all_zero(slot_state(3U).committed + 2U,
                    sizeof(slot_state(3U).committed) - 2U));

    zmk_runtime_macro_dynamic_clear_slot(3U);
    assert(all_zero(slot_state(3U).committed,
                    sizeof(slot_state(3U).committed)));

    /* Expiry zeroizes the buffer as well. */
    host_uptime = 2000;
    commit_slot(3U, long_text, sizeof(long_text) - 1U, 1U);
    host_uptime = 3000;
    zmk_runtime_macro_dynamic_check_expiry();
    assert_slot_empty(3U);
    assert(all_zero(slot_state(3U).committed,
                    sizeof(slot_state(3U).committed)));

    /* Staging is zeroized when a transaction is cancelled. */
    assert(zmk_runtime_macro_dynamic_begin_slot(3U, 4U, 30U, true) == 0);
    assert(zmk_runtime_macro_dynamic_append_slot(3U, 0U, short_text, 2U) == 0);
    zmk_runtime_macro_dynamic_cancel_staging();
    assert(all_zero(runtime_macro_dynamic_state.staging,
                    sizeof(runtime_macro_dynamic_state.staging)));
}

static void test_no_settings_or_static_slot_access(void) {
    const uint8_t text[] = "no persistence";

    reset_store();
    commit_slot(0U, text, sizeof(text) - 1U, 1U);
    commit_slot(7U, text, sizeof(text) - 1U, 1U);
    zmk_runtime_macro_dynamic_clear_slot(0U);
    zmk_runtime_macro_dynamic_clear_all();
    host_uptime = 100000;
    zmk_runtime_macro_dynamic_check_expiry();
    (void)zmk_runtime_macro_dynamic_execute_slot(7U);

    assert(settings_save_calls == 0U);
    assert(settings_delete_calls == 0U);
    assert(static_slot_set_calls == 0U);
    assert(static_slot_clear_calls == 0U);
}

struct race_context {
    const uint8_t *text;
    size_t length;
};

static void *append_final(void *arg) {
    const struct race_context *context = arg;
    const int err = zmk_runtime_macro_dynamic_append_slot(
        0U, ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN - 1U, context->text,
        context->length);
    assert(err == 0 || err == -ENOENT || err == -EINVAL);
    return NULL;
}

static void *clear_store(void *arg) {
    (void)arg;
    zmk_runtime_macro_dynamic_clear_all();
    return NULL;
}

static void test_clear_vs_final_append(void) {
    uint8_t text[ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN];
    uint8_t expected[ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN];
    const uint8_t final_byte = 'Z';
    const struct race_context context = {.text = &final_byte, .length = 1U};

    memset(text, 'Q', sizeof(text));
    memcpy(expected, text, sizeof(expected));
    expected[sizeof(expected) - 1U] = final_byte;

    for (size_t iteration = 0U; iteration < 100U; iteration++) {
        pthread_t append_thread;
        pthread_t clear_thread;

        reset_store();
        assert(zmk_runtime_macro_dynamic_begin_slot(
                   0U, sizeof(text), 300U, true) == 0);
        assert(zmk_runtime_macro_dynamic_append_slot(
                   0U, 0U, text, sizeof(text) - 1U) == 0);
        assert(pthread_create(&append_thread, NULL, append_final,
                              (void *)&context) == 0);
        assert(pthread_create(&clear_thread, NULL, clear_store, NULL) == 0);
        assert(pthread_join(append_thread, NULL) == 0);
        assert(pthread_join(clear_thread, NULL) == 0);

        assert(!runtime_macro_dynamic_state.staging_active);
        assert(slot_state(0U).committed_length == 0U ||
               slot_state(0U).committed_length == sizeof(text));
        if (slot_state(0U).committed_length == 0U) {
            assert_slot_empty(0U);
        } else {
            assert_slot_text(0U, expected, sizeof(expected));
        }
    }
}

int main(void) {
    test_reset_and_slot_layout();
    test_slot_indexing_and_bounds();
    test_length_boundaries();
    test_allowed_and_rejected_bytes();
    test_shared_staging_and_atomic_commit();
    test_legacy_slot_zero_entry_points();
    test_execution_policy_and_oversize();
    test_clear_slot_and_clear_all();
    test_per_slot_ttl_and_earliest_deadline();
    test_ttl_stale_generation();
    test_commit_replaces_ttl_and_zeroizes();
    test_clear_vs_final_append();
    test_no_settings_or_static_slot_access();
    puts("runtime macro dynamic store: PASS");
    return 0;
}
