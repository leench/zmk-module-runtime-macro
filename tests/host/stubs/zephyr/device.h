#pragma once

#include <stdbool.h>

struct device {
  const char *name;
  bool ready;
};

extern const struct device *device_get_binding(const char *name);
extern bool device_is_ready(const struct device *dev);
