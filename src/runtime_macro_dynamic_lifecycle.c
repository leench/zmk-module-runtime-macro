/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zmk/event_manager.h>

#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>

#include "runtime_macro_dynamic_internal.h"

static int runtime_macro_dynamic_profile_listener(const zmk_event_t *eh) {
  if (eh == NULL) {
    return -EINVAL;
  }

#if defined(CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_BLE_PROFILE_CHANGE) && \
    CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_BLE_PROFILE_CHANGE
  zmk_runtime_macro_dynamic_clear();
#endif

  return 0;
}

static int runtime_macro_dynamic_endpoint_listener(const zmk_event_t *eh) {
  if (eh == NULL) {
    return -EINVAL;
  }

#if defined(CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_ENDPOINT_CHANGE) && \
    CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_ENDPOINT_CHANGE
  zmk_runtime_macro_dynamic_clear();
#endif

  return 0;
}

#if defined(CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_BLE_PROFILE_CHANGE) && \
    CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_BLE_PROFILE_CHANGE
ZMK_LISTENER(runtime_macro_dynamic_profile, runtime_macro_dynamic_profile_listener);
ZMK_SUBSCRIPTION(runtime_macro_dynamic_profile, zmk_ble_active_profile_changed);
#endif

#if defined(CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_ENDPOINT_CHANGE) && \
    CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_ENDPOINT_CHANGE
ZMK_LISTENER(runtime_macro_dynamic_endpoint,
             runtime_macro_dynamic_endpoint_listener);
ZMK_SUBSCRIPTION(runtime_macro_dynamic_endpoint, zmk_endpoint_changed);
#endif
