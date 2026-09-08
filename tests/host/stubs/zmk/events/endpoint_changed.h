/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <zmk/event_manager.h>

struct zmk_endpoint_instance {
  uint8_t transport;
  uint8_t profile;
};

struct zmk_endpoint_changed {
  struct zmk_endpoint_instance endpoint;
};

static inline struct zmk_endpoint_changed *
as_zmk_endpoint_changed(const zmk_event_t *eh) {
  return (struct zmk_endpoint_changed *)eh;
}
