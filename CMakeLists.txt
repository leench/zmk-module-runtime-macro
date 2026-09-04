# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

# The runtime_macro behavior runs on a unibody keyboard or the central half of
# a split keyboard (slot storage and macro execution are central-side). Only
# create the library when the behavior itself is enabled, so builds without
# CONFIG_ZMK_BEHAVIOR_RUNTIME_MACRO do not end up with an empty library.
if (CONFIG_ZMK_BEHAVIOR_RUNTIME_MACRO
    AND ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
  zephyr_library()
  zephyr_library_include_directories(
    include
    ${APPLICATION_SOURCE_DIR}/include
  )
  zephyr_library_sources(
    src/runtime_macro.c
    src/runtime_macro_ascii.c
    src/runtime_macro_executor.c
    src/behaviors/behavior_runtime_macro.c
  )
endif() # CONFIG_ZMK_BEHAVIOR_RUNTIME_MACRO AND (unibody OR split central)
