/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/* Reuse the complete USB HID fixture with the disconnect policy disabled. */
#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_USB_DISCONNECT 0
#define main runtime_macro_usb_hid_policy_off_fixture_main
#include "runtime_macro_usb_hid_test.c"
#undef main

int main(void) { return runtime_macro_usb_hid_policy_off_fixture_main(); }
