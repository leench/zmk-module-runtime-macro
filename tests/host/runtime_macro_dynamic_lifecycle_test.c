/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/* Host-side tests for optional BLE profile and selected endpoint lifecycle
 * policies. */

#include <assert.h>
#include <errno.h>
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

#include "../../src/runtime_macro_dynamic.c"
#include "../../src/runtime_macro_dynamic_lifecycle.c"

static void commit_text(const char *text) {
  size_t length = strlen(text);
  assert(zmk_runtime_macro_dynamic_begin(
             length, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
  assert(zmk_runtime_macro_dynamic_append(0U, (const uint8_t *)text, length) ==
         0);
}

static void expect_committed(const char *text) {
  size_t length = strlen(text);
  assert(runtime_macro_dynamic_state.committed_valid);
  assert(runtime_macro_dynamic_state.committed_length == length);
  assert(memcmp(runtime_macro_dynamic_state.committed, text, length) == 0);
}

static void test_profile_and_endpoint_clear(void) {
  zmk_event_t event = {0};

  zmk_runtime_macro_dynamic_reset();
  commit_text("profile");
  assert(runtime_macro_dynamic_profile_listener(&event) == 0);
  assert(!runtime_macro_dynamic_state.committed_valid);
  assert(runtime_macro_dynamic_state.ttl_deadline_ms == 0);
  assert(!runtime_macro_dynamic_ttl_work.scheduled);

  commit_text("endpoint");
  assert(runtime_macro_dynamic_endpoint_listener(&event) == 0);
  assert(!runtime_macro_dynamic_state.committed_valid);
  assert(runtime_macro_dynamic_state.ttl_deadline_ms == 0);
  assert(!runtime_macro_dynamic_ttl_work.scheduled);
}

static void test_invalid_event_is_rejected(void) {
  commit_text("kept");
  assert(runtime_macro_dynamic_profile_listener(NULL) == -EINVAL);
  expect_committed("kept");
  assert(runtime_macro_dynamic_endpoint_listener(NULL) == -EINVAL);
  expect_committed("kept");
}

int main(void) {
  test_profile_and_endpoint_clear();
  test_invalid_event_is_rejected();
  return 0;
}
