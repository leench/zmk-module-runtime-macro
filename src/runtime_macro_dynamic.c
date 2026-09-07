/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>

#include "runtime_macro_dynamic_internal.h"

/*
 * Phase 1 reserves the two future RAM-only buffers so the feature-on map
 * measures their cost. No state transitions, locking, or data path exist yet.
 */
static volatile struct zmk_runtime_macro_dynamic_state
    runtime_macro_dynamic_state __attribute__((used));

_Static_assert(sizeof(runtime_macro_dynamic_state.committed) ==
                   ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN,
               "dynamic committed buffer size must match the fixed maximum");
_Static_assert(sizeof(runtime_macro_dynamic_state.staging) ==
                   ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN,
               "dynamic staging buffer size must match the fixed maximum");

static int runtime_macro_dynamic_skeleton_init(void) {
    /* Keep the reserved layout live until the phase-2 store owns it. */
    (void)runtime_macro_dynamic_state.committed[0];
    (void)runtime_macro_dynamic_state.staging[0];
    return 0;
}

SYS_INIT(runtime_macro_dynamic_skeleton_init, APPLICATION,
         CONFIG_APPLICATION_INIT_PRIORITY);
