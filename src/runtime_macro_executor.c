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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/events/keycode_state_changed.h>
#include <zmk/runtime_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum runtime_macro_executor_phase {
    RUNTIME_MACRO_EXECUTOR_PRESS,
    RUNTIME_MACRO_EXECUTOR_RELEASE,
};

struct runtime_macro_executor_state {
    char text[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN + 1];
    size_t length;
    size_t index;
    uint32_t encoded;
    enum runtime_macro_executor_phase phase;
};

static struct runtime_macro_executor_state runtime_macro_executor;
static atomic_t runtime_macro_executor_busy;

static void runtime_macro_executor_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(runtime_macro_executor_work, runtime_macro_executor_work_handler);

static void runtime_macro_executor_finish(void) {
    runtime_macro_executor.length = 0;
    runtime_macro_executor.index = 0;
    atomic_set(&runtime_macro_executor_busy, 0);
}

static int runtime_macro_executor_raise(bool pressed) {
    int err = raise_zmk_keycode_state_changed_from_encoded(runtime_macro_executor.encoded, pressed,
                                                           k_uptime_get());
    if (err != 0) {
        LOG_ERR("Runtime macro %s event failed (index %u, err %d)", pressed ? "press" : "release",
                runtime_macro_executor.index, err);
    }

    return err;
}

static int runtime_macro_executor_schedule(k_timeout_t delay) {
    int err = k_work_reschedule(&runtime_macro_executor_work, delay);
    if (err < 0) {
        LOG_ERR("Failed to schedule runtime macro work (err %d)", err);
    }

    return err;
}

static void runtime_macro_executor_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (atomic_get(&runtime_macro_executor_busy) == 0) {
        return;
    }

    switch (runtime_macro_executor.phase) {
    case RUNTIME_MACRO_EXECUTOR_PRESS: {
        int err = zmk_runtime_macro_ascii_to_keycode(
            (uint8_t)runtime_macro_executor.text[runtime_macro_executor.index],
            &runtime_macro_executor.encoded);
        if (err != 0) {
            LOG_ERR("Runtime macro contains invalid byte at index %u (err %d)",
                    runtime_macro_executor.index, err);
            runtime_macro_executor_finish();
            return;
        }

        /* Always move to RELEASE, even if the press listener reports an error. */
        (void)runtime_macro_executor_raise(true);
        runtime_macro_executor.phase = RUNTIME_MACRO_EXECUTOR_RELEASE;

        err = runtime_macro_executor_schedule(K_MSEC(CONFIG_ZMK_RUNTIME_MACRO_TAP_MS));
        if (err < 0) {
            /* A successful press may already have reached a listener. */
            (void)runtime_macro_executor_raise(false);
            runtime_macro_executor_finish();
        }
        return;
    }

    case RUNTIME_MACRO_EXECUTOR_RELEASE:
        (void)runtime_macro_executor_raise(false);
        runtime_macro_executor.index++;

        if (runtime_macro_executor.index >= runtime_macro_executor.length) {
            runtime_macro_executor_finish();
            return;
        }

        runtime_macro_executor.phase = RUNTIME_MACRO_EXECUTOR_PRESS;
        int err = runtime_macro_executor_schedule(K_MSEC(CONFIG_ZMK_RUNTIME_MACRO_WAIT_MS));
        if (err < 0) {
            /* The current key was released, so it is safe to finish here. */
            runtime_macro_executor_finish();
        }
        return;
    }
}

int zmk_runtime_macro_execute(uint8_t slot) {
    char snapshot[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN + 1];
    size_t length;

    int err = zmk_runtime_macro_slot_copy(slot, snapshot, sizeof(snapshot), &length);
    if (err != 0) {
        return err;
    }

    if (!atomic_cas(&runtime_macro_executor_busy, 0, 1)) {
        return -EBUSY;
    }

    if (length == 0) {
        atomic_set(&runtime_macro_executor_busy, 0);
        return 0;
    }

    memcpy(runtime_macro_executor.text, snapshot, length + 1);
    runtime_macro_executor.length = length;
    runtime_macro_executor.index = 0;
    runtime_macro_executor.phase = RUNTIME_MACRO_EXECUTOR_PRESS;

    err = k_work_schedule(&runtime_macro_executor_work, K_NO_WAIT);
    if (err < 0) {
        LOG_ERR("Failed to start runtime macro work (err %d)", err);
        runtime_macro_executor_finish();
        return err;
    }

    return 0;
}

bool zmk_runtime_macro_is_busy(void) { return atomic_get(&runtime_macro_executor_busy) != 0; }
