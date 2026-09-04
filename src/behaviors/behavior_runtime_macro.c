/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_runtime_macro

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/runtime_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param1_values[] = {
    {
        .display_name = "Slot",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range =
            {
                .min = 0,
                .max = CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT - 1,
            },
    },
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param1_values,
    .param1_values_len = ARRAY_SIZE(param1_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static bool runtime_macro_slot_is_valid(uint32_t slot) {
    return slot < CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (!runtime_macro_slot_is_valid(binding->param1)) {
        LOG_ERR("Runtime macro slot %u out of range (max %d)", binding->param1,
                CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT - 1);
        return -EINVAL;
    }

    int err = zmk_runtime_macro_execute((uint8_t)binding->param1);
    if (err != 0) {
        LOG_ERR("Failed to start runtime macro slot %u (err %d)", binding->param1, err);
        return err;
    }

    LOG_DBG("position %d started runtime macro slot %u", event.position, binding->param1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    if (!runtime_macro_slot_is_valid(binding->param1)) {
        LOG_ERR("Runtime macro slot %u out of range (max %d)", binding->param1,
                CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT - 1);
        return -EINVAL;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_runtime_macro_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define RM_INST(n)                                                                                 \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_runtime_macro_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RM_INST)

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
