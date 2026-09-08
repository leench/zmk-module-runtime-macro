/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/devicetree.h>

/*
 * Fail fast when a keymap references the dynamic behavior while its feature
 * gate cannot provide the driver (for example, on a peripheral or without
 * management USB HID).  /omit-if-no-ref/ keeps this check inactive for
 * static-only keymaps, including keep-all builds.
 */
#if CONFIG_DT_HAS_ZMK_BEHAVIOR_RUNTIME_MACRO_DYNAMIC_ENABLED && \
    !CONFIG_ZMK_BEHAVIOR_RUNTIME_MACRO_DYNAMIC
#error "runtime_macro_dynamic is referenced but its feature/driver is disabled"
#endif
