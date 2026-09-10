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

/*
 * Per-slot committed, staging, and executor capacity of the multi-slot store.
 * The shared executor snapshot can hold the full per-slot maximum since D2.
 */
#define ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN 512U

/*
 * Legacy single-object bound of the frozen v1 dynamic wire contract. The v1
 * protocol opcodes, DYNAMIC_PROTOCOL.md, and the legacy single-object entry
 * points all still address one 256-byte object on the legacy slot until D3
 * migrates the wire to per-slot addressing. The multi-slot store and the
 * parameterized behavior use ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN.
 */
#define ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN 256U

/* Host test builds include this header without generated Kconfig values. */
#if !defined(CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT)
#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT 8
#endif

#define ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT \
    CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT

/*
 * Slot used by the legacy single-object entry points. The frozen v1 wire
 * contract, the legacy single-object entry points, and the lifecycle clear
 * paths address this slot until D3 migrates the wire to per-slot addressing.
 * The parameterized behavior addresses explicit slots since D2.
 */
#define ZMK_RUNTIME_MACRO_DYNAMIC_LEGACY_SLOT 0U

#define ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS 300U
#define ZMK_RUNTIME_MACRO_DYNAMIC_MIN_TTL_SECONDS 1U
#define ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TTL_SECONDS 86400U

_Static_assert(ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN == 512U,
               "multi-slot dynamic macro maximum must remain 512 bytes");
_Static_assert(ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN == 256U,
               "the legacy dynamic macro bound must remain 256 bytes");
_Static_assert(ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT >= 1U &&
                   ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT <= 8U,
               "dynamic macro slot count must stay within 1..8");

/*
 * One independent dynamic slot. Fields are ordered to avoid padding between
 * the buffer and the per-slot metadata.
 */
struct zmk_runtime_macro_dynamic_slot {
    uint8_t committed[ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN];
    int64_t ttl_deadline_ms;
    uint32_t ttl_generation;
    uint16_t committed_length;
    bool committed_valid;
    bool committed_consume_on_accept;
};

/*
 * RAM-only dynamic macro state. Committed text is per slot; staging is a
 * single shared buffer, so exactly one upload transaction can be active. The
 * buffers are deliberately kept separate from the Settings-backed static slot
 * store.
 */
struct zmk_runtime_macro_dynamic_state {
    struct zmk_runtime_macro_dynamic_slot
        slots[ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT];

    uint8_t staging[ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN];
    uint8_t staging_slot;
    size_t staging_expected_length;
    size_t staging_received;
    bool staging_active;
    uint32_t staging_ttl_seconds;
    bool staging_consume_on_accept;

    /*
     * Single delayable TTL work item for every slot. ttl_generation is a
     * monotonic counter bumped on every TTL-affecting change;
     * ttl_work_generation records the generation the work item was last armed
     * for, so a work item armed before a newer commit cannot expire it.
     */
    uint32_t ttl_generation;
    uint32_t ttl_work_generation;
};

_Static_assert(
    sizeof(((struct zmk_runtime_macro_dynamic_state *)0)->slots[0].committed) ==
        ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN,
    "dynamic slot buffer must match the fixed per-slot maximum");
_Static_assert(
    sizeof(((struct zmk_runtime_macro_dynamic_state *)0)->staging) ==
        ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN,
    "dynamic staging buffer must match the fixed per-slot maximum");

/*
 * Reset all volatile state and cancel any pending TTL work. This is the only
 * initialization path; reboot/reset therefore starts with empty slots.
 */
void zmk_runtime_macro_dynamic_reset(void);

/* Number of addressable dynamic slots (1..8). */
uint8_t zmk_runtime_macro_dynamic_slot_count(void);

/*
 * Start a staging transaction for one slot. A valid begin always cancels the
 * previous in-progress transaction, whichever slot it targeted, while
 * preserving every committed slot and its TTL. total_length must be 1..512 and
 * ttl_seconds must be 1..86400. Invalid input cancels current staging and
 * preserves every committed slot. Returns -EINVAL for invalid arguments.
 */
int zmk_runtime_macro_dynamic_begin_slot(uint8_t slot, size_t total_length,
                                         uint32_t ttl_seconds,
                                         bool consume_on_accept);

/*
 * Append one contiguous chunk to the active staging transaction. slot must
 * match the slot recorded by begin; offset must equal the current received
 * count, and length must be 1..(expected - received). Every byte is checked
 * against the existing macro alphabet: 0x20..0x7e, LF, Tab, or Backspace. A
 * bad request cancels staging and preserves every committed slot. The final
 * chunk is committed to its slot atomically and starts that slot's TTL.
 * Returns -EINVAL for bad arguments, -ENOENT when no transaction is active,
 * and 0 on success.
 */
int zmk_runtime_macro_dynamic_append_slot(uint8_t slot, size_t offset,
                                          const uint8_t *data, size_t length);

/* Cancel only the in-progress staging transaction; committed slots are kept. */
void zmk_runtime_macro_dynamic_cancel_staging(void);

/*
 * Clear one slot's committed text and TTL, and cancel staging when it targets
 * that slot. Out-of-range slots are ignored. Other slots are never affected.
 */
void zmk_runtime_macro_dynamic_clear_slot(uint8_t slot);

/*
 * Clear every slot, the staging transaction, and all TTL deadlines. This is a
 * lifecycle operation and is used by reboot-style reset paths.
 */
void zmk_runtime_macro_dynamic_clear_all(void);

/*
 * Check the current uptime and expire every committed slot whose TTL has
 * elapsed. The delayable work handler calls the same path.
 */
void zmk_runtime_macro_dynamic_check_expiry(void);

/*
 * Try to hand one slot's committed text to the shared executor. Empty or
 * expired text is harmless and returns 0. By default that slot is consumed
 * when the executor accepts its private snapshot; an upload may opt out of
 * consumption. Busy or start failures always keep it available, and an
 * out-of-range slot is refused with -EINVAL without touching any slot. The
 * shared executor can hold the full per-slot maximum.
 */
int zmk_runtime_macro_dynamic_execute_slot(uint8_t slot);

/*
 * Legacy single-object entry points. They act on
 * ZMK_RUNTIME_MACRO_DYNAMIC_LEGACY_SLOT and keep the frozen 256-byte bound of
 * the v1 wire contract. They remain the entry points used by the protocol and
 * USB/lifecycle code until D3 migrates the wire to explicit slots; the keymap
 * behavior addresses explicit slots since D2.
 */
int zmk_runtime_macro_dynamic_begin(size_t total_length, uint32_t ttl_seconds);
int zmk_runtime_macro_dynamic_begin_with_options(size_t total_length,
                                                 uint32_t ttl_seconds,
                                                 bool consume_on_accept);
int zmk_runtime_macro_dynamic_append(size_t offset, const uint8_t *data,
                                     size_t length);
int zmk_runtime_macro_dynamic_execute(void);

/*
 * Clear the whole dynamic state (every slot, staging, and all TTL deadlines).
 * This matches the v1 whole-state clear semantics of the frozen wire contract
 * and of the USB/profile/endpoint lifecycle policies.
 */
void zmk_runtime_macro_dynamic_clear(void);
