/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/* Host-side tests for the default-preserve BLE profile and selected endpoint
 * lifecycle policies. */

#include <assert.h>
#include <errno.h>
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

#include "../../src/runtime_macro_dynamic.c"
#include "../../src/runtime_macro_dynamic_lifecycle.c"

static void commit_text(const char *text) {
  size_t length = strlen(text);
  assert(zmk_runtime_macro_dynamic_begin(
             length, ZMK_RUNTIME_MACRO_DYNAMIC_DEFAULT_TTL_SECONDS) == 0);
  assert(zmk_runtime_macro_dynamic_append(0U, (const uint8_t *)text, length) ==
         0);
}

static void test_default_preservation(void) {
  zmk_event_t event = {0};

  zmk_runtime_macro_dynamic_reset();
  commit_text("profile");
  assert(runtime_macro_dynamic_profile_listener(&event) == 0);
  assert(runtime_macro_dynamic_state.slots[0U].committed_valid);
  assert(runtime_macro_dynamic_state.slots[0U].committed_length == 7U);

  commit_text("endpoint");
  assert(runtime_macro_dynamic_endpoint_listener(&event) == 0);
  assert(runtime_macro_dynamic_state.slots[0U].committed_valid);
  assert(runtime_macro_dynamic_state.slots[0U].committed_length == 8U);
}

static void test_invalid_event_is_rejected(void) {
  commit_text("kept");
  assert(runtime_macro_dynamic_profile_listener(NULL) == -EINVAL);
  assert(runtime_macro_dynamic_endpoint_listener(NULL) == -EINVAL);
  assert(runtime_macro_dynamic_state.slots[0U].committed_valid);
  assert(runtime_macro_dynamic_state.slots[0U].committed_length == 4U);
}

int main(void) {
  test_default_preservation();
  test_invalid_event_is_rejected();
  return 0;
}
