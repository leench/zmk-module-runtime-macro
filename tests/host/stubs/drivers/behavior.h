#pragma once

#include <zmk/behavior.h>

struct behavior_driver_api;

typedef int (*behavior_keymap_binding_callback_t)(struct zmk_behavior_binding *binding,
                                                  struct zmk_behavior_binding_event event);

enum behavior_locality {
    BEHAVIOR_LOCALITY_CENTRAL,
    BEHAVIOR_LOCALITY_EVENT_SOURCE,
    BEHAVIOR_LOCALITY_GLOBAL,
};

struct behavior_driver_api {
    enum behavior_locality locality;
    behavior_keymap_binding_callback_t binding_pressed;
    behavior_keymap_binding_callback_t binding_released;
};

#define BEHAVIOR_DT_INST_DEFINE(...) ((void)0)
