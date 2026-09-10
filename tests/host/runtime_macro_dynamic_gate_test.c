/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>

#include "../../src/runtime_macro_dynamic_internal.h"

int main(void) {
    struct zmk_runtime_macro_dynamic_state state = {0};

    assert(ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN == 256U);
    assert(ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN == 512U);
    assert(ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT == 8U);
    assert(sizeof(state.slots[0].committed) == 512U);
    assert(sizeof(state.staging) == 512U);

    puts("runtime macro dynamic gate constants: PASS");
    return 0;
}
