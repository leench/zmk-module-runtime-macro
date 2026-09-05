/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>

#include <zephyr/settings/settings.h>

/* Settings bridge used by runtime_macro.c; not part of the public auth API. */
int zmk_runtime_macro_auth_settings_set(const char *name, size_t length,
                                        settings_read_cb read_cb, void *cb_arg);
int zmk_runtime_macro_auth_settings_commit(void);
