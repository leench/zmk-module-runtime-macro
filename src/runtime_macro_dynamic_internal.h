/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#define ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN 256U

_Static_assert(ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN == 256U,
               "dynamic macro maximum must remain 256 bytes");

/*
 * Phase 1 layout skeleton only. State transitions, synchronization, TTL, and
 * all public/internal operations are intentionally added in later phases.
 */
struct zmk_runtime_macro_dynamic_state {
    uint8_t committed[ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN];
    uint8_t staging[ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN];
};

_Static_assert(sizeof(((struct zmk_runtime_macro_dynamic_state *)0)->committed) ==
                   256U,
               "dynamic committed buffer must remain 256 bytes");
_Static_assert(sizeof(((struct zmk_runtime_macro_dynamic_state *)0)->staging) ==
                   256U,
               "dynamic staging buffer must remain 256 bytes");
