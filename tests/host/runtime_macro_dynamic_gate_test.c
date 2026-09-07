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
    assert(sizeof(state.committed) == 256U);
    assert(sizeof(state.staging) == 256U);

    puts("runtime macro dynamic gate constants: PASS");
    return 0;
}
