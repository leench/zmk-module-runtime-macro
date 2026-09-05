#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t unused;
} zmk_event_t;

#define ZMK_LISTENER(mod, cb)                                                   \
  static int (*const zmk_test_listener_##mod)(const zmk_event_t *)              \
      __attribute__((unused)) = (cb)
#define ZMK_SUBSCRIPTION(mod, ev_type)                                         \
  static int zmk_test_subscription_##mod##_##ev_type __attribute__((unused)) = 0
