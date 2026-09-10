/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Host-side tests for the opt-in BLE profile and selected endpoint lifecycle
 * policies.
 *
 * Both policies clear every dynamic slot, the single shared staging
 * transaction, and the pending TTL work. They must never reach the
 * Settings-backed static store, and they must remain independent of the
 * authentication paths.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC 1
#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_BLE_PROFILE_CHANGE 1
#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_ENDPOINT_CHANGE 1

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

#include "../../src/runtime_macro_ascii.c"

int64_t host_uptime;

int host_work_schedule(struct k_work_delayable *work, k_timeout_t delay,
                       bool reschedule) {
  (void)reschedule;
  work->scheduled = true;
  work->delay = delay;
  return 0;
}

int zmk_runtime_macro_executor_start(const uint8_t *text, size_t length) {
  (void)text;
  (void)length;
  return 0;
}

/*
 * The lifecycle clears are dynamic-only. The stubs record every call so the
 * test can assert that the Settings and static-slot paths stay untouched.
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

#include "../../src/runtime_macro_dynamic.c"
#include "../../src/runtime_macro_dynamic_lifecycle.c"

#define SLOT_COUNT ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT

static bool all_zero(const uint8_t *buffer, size_t length) {
  for (size_t index = 0U; index < length; index++) {
    if (buffer[index] != 0U) {
      return false;
    }
  }

  return true;
}

static void reset_state(void) {
  zmk_runtime_macro_dynamic_reset();
  host_uptime = 0;
  settings_save_calls = 0U;
  settings_delete_calls = 0U;
  static_slot_set_calls = 0U;
  static_slot_clear_calls = 0U;
}

static void commit_slot_text_with_policy(uint8_t slot, const char *text,
                                         bool consume_on_accept) {
  size_t length = strlen(text);

  assert(zmk_runtime_macro_dynamic_begin_slot(
             slot, length, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS,
             consume_on_accept) == 0);
  assert(zmk_runtime_macro_dynamic_append_slot(
             slot, 0U, (const uint8_t *)text, length) == 0);
}

static void commit_slot_text(uint8_t slot, const char *text) {
  commit_slot_text_with_policy(slot, text, true);
}

static void expect_slot_text(uint8_t slot, const char *text) {
  size_t length = strlen(text);

  assert(runtime_macro_dynamic_state.slots[slot].committed_valid);
  assert(runtime_macro_dynamic_state.slots[slot].committed_length == length);
  assert(memcmp(runtime_macro_dynamic_state.slots[slot].committed, text,
                length) == 0);
}

static void expect_slot_empty(uint8_t slot) {
  assert(!runtime_macro_dynamic_state.slots[slot].committed_valid);
  assert(runtime_macro_dynamic_state.slots[slot].committed_length == 0U);
  assert(runtime_macro_dynamic_state.slots[slot].ttl_deadline_ms == 0);
  assert(runtime_macro_dynamic_state.slots[slot].committed_consume_on_accept);
  assert(all_zero(runtime_macro_dynamic_state.slots[slot].committed,
                  sizeof(runtime_macro_dynamic_state.slots[slot].committed)));
}

static void expect_all_slots_empty(void) {
  for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
    expect_slot_empty(slot);
  }
}

static void expect_staging_empty(void) {
  assert(!runtime_macro_dynamic_state.staging_active);
  assert(runtime_macro_dynamic_state.staging_slot == 0U);
  assert(runtime_macro_dynamic_state.staging_expected_length == 0U);
  assert(runtime_macro_dynamic_state.staging_received == 0U);
  assert(all_zero(runtime_macro_dynamic_state.staging,
                  sizeof(runtime_macro_dynamic_state.staging)));
}

/* Every slot holds text and one shared upload is in progress. */
static void test_profile_clears_every_slot(void) {
  zmk_event_t event = {0};
  char per_slot[2];

  reset_state();

  for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
    per_slot[0] = (char)('a' + slot);
    per_slot[1] = '\0';
    commit_slot_text(slot, per_slot);
  }
  assert(runtime_macro_dynamic_ttl_work.scheduled);

  assert(zmk_runtime_macro_dynamic_begin_slot(
             2U, 4U, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS, true) == 0);
  assert(zmk_runtime_macro_dynamic_append_slot(2U, 0U,
                                               (const uint8_t *)"ab", 2U) == 0);
  assert(runtime_macro_dynamic_state.staging_active);

  assert(runtime_macro_dynamic_profile_listener(&event) == 0);

  expect_all_slots_empty();
  expect_staging_empty();
  assert(!runtime_macro_dynamic_ttl_work.scheduled);
}

/*
 * Boundary state: only some slots hold text, one committed slot is already
 * past its deadline, one upload targets an otherwise empty slot, and the
 * remaining slots are empty.
 */
static void test_endpoint_clears_partial_state(void) {
  zmk_event_t event = {0};

  reset_state();

  /* Slot 1 is committed with a 1-second TTL and is already expired. */
  assert(zmk_runtime_macro_dynamic_begin_slot(1U, 7U, 1U, true) == 0);
  assert(zmk_runtime_macro_dynamic_append_slot(1U, 0U,
                                               (const uint8_t *)"expired",
                                               7U) == 0);
  commit_slot_text_with_policy(4U, "keep", false);
  commit_slot_text(7U, "plain");

  host_uptime = 5000;
  assert(runtime_macro_dynamic_state.slots[1U].ttl_deadline_ms == 1000);
  assert(zmk_runtime_macro_dynamic_begin_slot(
             5U, 4U, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS, true) == 0);
  assert(zmk_runtime_macro_dynamic_append_slot(5U, 0U,
                                               (const uint8_t *)"xy", 2U) == 0);

  assert(runtime_macro_dynamic_endpoint_listener(&event) == 0);

  expect_all_slots_empty();
  expect_staging_empty();
  assert(!runtime_macro_dynamic_ttl_work.scheduled);

  /* The policy is idempotent on an already empty state. */
  assert(runtime_macro_dynamic_endpoint_listener(&event) == 0);
  expect_all_slots_empty();
  expect_staging_empty();
  assert(!runtime_macro_dynamic_ttl_work.scheduled);
}

/* The boot path is the volatile initialization reset; it must clear every
 * slot, staging, and the pending TTL work. */
static void test_boot_reset_clears_every_slot(void) {
  reset_state();

  for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
    commit_slot_text(slot, "boot");
  }
  assert(zmk_runtime_macro_dynamic_begin_slot(
             3U, 4U, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS, true) == 0);
  assert(runtime_macro_dynamic_ttl_work.scheduled);

  zmk_runtime_macro_dynamic_reset();

  expect_all_slots_empty();
  expect_staging_empty();
  assert(!runtime_macro_dynamic_ttl_work.scheduled);
}

static void test_invalid_event_is_rejected(void) {
  reset_state();

  commit_slot_text(0U, "kept");
  commit_slot_text(6U, "kept-too");
  assert(zmk_runtime_macro_dynamic_begin_slot(
             1U, 4U, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS, true) == 0);
  assert(zmk_runtime_macro_dynamic_append_slot(1U, 0U,
                                               (const uint8_t *)"ab", 2U) == 0);

  assert(runtime_macro_dynamic_profile_listener(NULL) == -EINVAL);
  assert(runtime_macro_dynamic_endpoint_listener(NULL) == -EINVAL);

  expect_slot_text(0U, "kept");
  expect_slot_text(6U, "kept-too");
  assert(!runtime_macro_dynamic_state.slots[1U].committed_valid);
  assert(!runtime_macro_dynamic_state.slots[2U].committed_valid);
  assert(runtime_macro_dynamic_state.staging_active);
  assert(runtime_macro_dynamic_state.staging_slot == 1U);
  assert(runtime_macro_dynamic_state.staging_received == 2U);
}

static void test_no_settings_or_static_slot_access(void) {
  zmk_event_t event = {0};

  reset_state();

  commit_slot_text(0U, "dynamic");
  commit_slot_text(7U, "dynamic-too");
  assert(runtime_macro_dynamic_profile_listener(&event) == 0);
  commit_slot_text(0U, "dynamic");
  assert(runtime_macro_dynamic_endpoint_listener(&event) == 0);

  assert(settings_save_calls == 0U);
  assert(settings_delete_calls == 0U);
  assert(static_slot_set_calls == 0U);
  assert(static_slot_clear_calls == 0U);
}

int main(void) {
  test_profile_clears_every_slot();
  test_endpoint_clears_partial_state();
  test_boot_reset_clears_every_slot();
  test_invalid_event_is_rejected();
  test_no_settings_or_static_slot_access();
  return 0;
}
