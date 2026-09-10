/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Host-side tests for the default-preserve BLE profile and selected endpoint
 * lifecycle policies.
 *
 * With both policies disabled the listeners are not even registered, and the
 * callback bodies must preserve every dynamic slot, the shared staging
 * transaction, and the pending TTL work. The stubs also record that the
 * Settings and static-slot paths stay untouched.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC 1

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

static void reset_state(void) {
  zmk_runtime_macro_dynamic_reset();
  host_uptime = 0;
  settings_save_calls = 0U;
  settings_delete_calls = 0U;
  static_slot_set_calls = 0U;
  static_slot_clear_calls = 0U;
}

static void commit_slot_text(uint8_t slot, const char *text) {
  size_t length = strlen(text);

  assert(zmk_runtime_macro_dynamic_begin_slot(
             slot, length, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS,
             true) == 0);
  assert(zmk_runtime_macro_dynamic_append_slot(
             slot, 0U, (const uint8_t *)text, length) == 0);
}

static void expect_slot_text(uint8_t slot, const char *text) {
  size_t length = strlen(text);

  assert(runtime_macro_dynamic_state.slots[slot].committed_valid);
  assert(runtime_macro_dynamic_state.slots[slot].committed_length == length);
  assert(memcmp(runtime_macro_dynamic_state.slots[slot].committed, text,
                length) == 0);
}

static void expect_unused_slots_empty(void) {
  for (uint8_t slot = 0U; slot < (uint8_t)SLOT_COUNT; slot++) {
    if (slot == 1U || slot == 3U || slot == 7U) {
      continue;
    }

    assert(!runtime_macro_dynamic_state.slots[slot].committed_valid);
    assert(runtime_macro_dynamic_state.slots[slot].committed_length == 0U);
  }
}

/* Every slot holds text and one shared upload is in progress. */
static void test_default_preservation_all_slots(void) {
  zmk_event_t event = {0};

  reset_state();

  commit_slot_text(1U, "profile");
  commit_slot_text(3U, "middle");
  commit_slot_text(7U, "last");

  assert(zmk_runtime_macro_dynamic_begin_slot(
             5U, 4U, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS, true) == 0);
  assert(zmk_runtime_macro_dynamic_append_slot(5U, 0U,
                                               (const uint8_t *)"ab", 2U) == 0);
  assert(runtime_macro_dynamic_ttl_work.scheduled);

  assert(runtime_macro_dynamic_profile_listener(&event) == 0);

  expect_slot_text(1U, "profile");
  expect_slot_text(3U, "middle");
  expect_slot_text(7U, "last");
  expect_unused_slots_empty();
  assert(runtime_macro_dynamic_state.staging_active);
  assert(runtime_macro_dynamic_state.staging_slot == 5U);
  assert(runtime_macro_dynamic_state.staging_received == 2U);
  assert(runtime_macro_dynamic_ttl_work.scheduled);

  assert(runtime_macro_dynamic_endpoint_listener(&event) == 0);

  expect_slot_text(1U, "profile");
  expect_slot_text(3U, "middle");
  expect_slot_text(7U, "last");
  expect_unused_slots_empty();
  assert(runtime_macro_dynamic_state.staging_active);
  assert(runtime_macro_dynamic_state.staging_slot == 5U);
  assert(runtime_macro_dynamic_state.staging_received == 2U);
  assert(runtime_macro_dynamic_ttl_work.scheduled);

  assert(settings_save_calls == 0U);
  assert(settings_delete_calls == 0U);
  assert(static_slot_set_calls == 0U);
  assert(static_slot_clear_calls == 0U);
}

static void test_invalid_event_is_rejected(void) {
  reset_state();

  commit_slot_text(2U, "kept");
  assert(runtime_macro_dynamic_profile_listener(NULL) == -EINVAL);
  assert(runtime_macro_dynamic_endpoint_listener(NULL) == -EINVAL);
  expect_slot_text(2U, "kept");
}

int main(void) {
  test_default_preservation_all_slots();
  test_invalid_event_is_rejected();
  return 0;
}
