#!/bin/sh

# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${TMPDIR:-/tmp}/zmk-runtime-macro-tests
mkdir -p "$build_dir"

common_cflags="-std=c11 -Wall -Wextra -Werror -pedantic -pthread -I$root_dir/tests/host/stubs -I$root_dir/include"
ble_policy_block=$(sed -n '/^config ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_BLE_PROFILE_CHANGE$/,/^config /p' "$root_dir/Kconfig")
if ! printf '%s\n' "$ble_policy_block" | grep -q 'depends on ZMK_BLE'; then
    echo "runtime macro BLE lifecycle policy gate: FAIL"
    exit 1
fi
printf '%s\n' "runtime macro BLE lifecycle policy gate: PASS"

dynamic_slot_block=$(sed -n '/^config ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT$/,/^config /p' "$root_dir/Kconfig")
if ! printf '%s\n' "$dynamic_slot_block" | grep -q 'depends on ZMK_RUNTIME_MACRO_DYNAMIC'; then
    echo "runtime macro dynamic slot count dependency: FAIL"
    exit 1
fi
if ! printf '%s\n' "$dynamic_slot_block" | grep -q 'default 8' ||
   ! printf '%s\n' "$dynamic_slot_block" | grep -q 'range 1 8'; then
    echo "runtime macro dynamic slot count range: FAIL"
    exit 1
fi
printf '%s\n' "runtime macro dynamic slot count gate: PASS"

test_sources="runtime_macro_slots_test runtime_macro_executor_test runtime_macro_protocol_test runtime_macro_protocol_no_ble_policy_test runtime_macro_usb_hid_test runtime_macro_usb_hid_policy_off_test runtime_macro_auth_test runtime_macro_auth_protocol_test runtime_macro_dynamic_gate_test runtime_macro_dynamic_store_test runtime_macro_dynamic_behavior_test runtime_macro_dynamic_lifecycle_test runtime_macro_dynamic_lifecycle_policy_off_test"

test_suite() {
    compiler=$1
    suite_name=$2
    extra_cflags=$3

    for test_name in $test_sources; do
        # shellcheck disable=SC2086
        "$compiler" $common_cflags $extra_cflags \
            "$root_dir/tests/host/$test_name.c" \
            -o "$build_dir/${suite_name}_${test_name}"
    done

    for test_name in $test_sources; do
        "$build_dir/${suite_name}_${test_name}"
    done
}

gcc_bin=${GCC:-gcc}
clang_bin=${CLANG:-clang}

# shellcheck disable=SC2086
test_suite "$gcc_bin" gcc ""
test_suite "$gcc_bin" gcc_sanitize "-fsanitize=address,undefined -fno-omit-frame-pointer -g"
test_suite "$clang_bin" clang ""
test_suite "$clang_bin" clang_sanitize "-fsanitize=address,undefined -fno-omit-frame-pointer -g"
