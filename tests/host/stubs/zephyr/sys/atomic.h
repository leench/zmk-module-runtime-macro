#pragma once

#include <zephyr/kernel.h>

#define ATOMIC_INIT(value) (value)

static inline atomic_val_t atomic_inc(atomic_t *target) {
  atomic_val_t old_value = *target;
  *target = old_value + 1;
  return old_value;
}
