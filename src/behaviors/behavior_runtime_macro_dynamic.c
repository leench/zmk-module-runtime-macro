/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_macro_dynamic

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include "../runtime_macro_dynamic_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const uint32_t slot = binding->param1;

    /* The log statements can compile out, so keep the parameter marked used. */
    ARG_UNUSED(event);

    /*
     * The binding cell is a full 32-bit devicetree value: range-check it before
     * narrowing, otherwise an out-of-range cell could wrap onto a valid slot
     * and execute the wrong macro. An out-of-range slot is rejected without
     * executing, consuming, or modifying any slot.
     */
    if (slot >= (uint32_t)ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT) {
        LOG_ERR("Dynamic runtime macro slot %u is out of range (position %d)", slot,
                event.position);
        return -EINVAL;
    }

    int err = zmk_runtime_macro_dynamic_execute_slot((uint8_t)slot);
    if (err != 0) {
        LOG_ERR("Failed to start dynamic runtime macro slot %u (position %d, err %d)", slot,
                event.position, err);
        return err;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_runtime_macro_dynamic_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define RMD_INST(n)                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                           \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                              \
                            &behavior_runtime_macro_dynamic_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RMD_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
