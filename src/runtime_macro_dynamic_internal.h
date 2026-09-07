/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#define ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN 256U
#define ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS 300U
#define ZMK_RUNTIME_MACRO_DYNAMIC_MIN_TTL_SECONDS 1U
#define ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TTL_SECONDS 86400U

_Static_assert(ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN == 256U,
               "dynamic macro maximum must remain 256 bytes");

/*
 * RAM-only dynamic macro state. The two bounded buffers are deliberately kept
 * separate from the Settings-backed static slot store.
 */
struct zmk_runtime_macro_dynamic_state {
    uint8_t committed[ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN];
    size_t committed_length;
    bool committed_valid;

    uint8_t staging[ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN];
    size_t staging_expected_length;
    size_t staging_received;
    bool staging_active;
    uint32_t staging_ttl_seconds;

    int64_t ttl_deadline_ms;
    uint32_t ttl_generation;
    uint32_t ttl_work_generation;
};

_Static_assert(sizeof(((struct zmk_runtime_macro_dynamic_state *)0)->committed) ==
                   256U,
               "dynamic committed buffer must remain 256 bytes");
_Static_assert(sizeof(((struct zmk_runtime_macro_dynamic_state *)0)->staging) ==
                   256U,
               "dynamic staging buffer must remain 256 bytes");

/*
 * Reset all volatile state and cancel any pending TTL work. This is the only
 * initialization path; reboot/reset therefore starts with empty buffers.
 */
void zmk_runtime_macro_dynamic_reset(void);

/*
 * Start a new staging transaction. A valid begin always replaces old staging,
 * while preserving committed text and its TTL. total_length must be 1..256;
 * ttl_seconds must be 1..86400. Invalid input cancels current staging and
 * preserves committed text. Returns -EINVAL for invalid arguments.
 */
int zmk_runtime_macro_dynamic_begin(size_t total_length, uint32_t ttl_seconds);

/*
 * Append one contiguous chunk. offset must equal the current received count,
 * and length must be 1..(expected - received). Every byte is checked against
 * the existing macro alphabet: 0x20..0x7e, LF, Tab, or Backspace. A bad
 * request cancels staging and preserves committed text. The final chunk is
 * committed atomically and starts its TTL. Returns -EINVAL for bad arguments,
 * -ENOENT when no transaction is active, and 0 on success.
 */
int zmk_runtime_macro_dynamic_append(size_t offset, const uint8_t *data,
                                     size_t length);

/* Cancel only the in-progress staging transaction; committed text is kept. */
void zmk_runtime_macro_dynamic_cancel_staging(void);

/* Clear committed text, staging, and the active TTL. This operation is idempotent. */
void zmk_runtime_macro_dynamic_clear(void);

/*
 * Check the current uptime and expire the committed object if its TTL has
 * elapsed. The delayable work handler calls the same path; this function is
 * also used by later lifecycle code before observing dynamic state.
 */
void zmk_runtime_macro_dynamic_check_expiry(void);
