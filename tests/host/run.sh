#!/bin/sh

# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${TMPDIR:-/tmp}/zmk-runtime-macro-tests
mkdir -p "$build_dir"

cc_bin=${CC:-cc}
common_cflags="-std=c11 -Wall -Wextra -Werror -pedantic -pthread -I$root_dir/tests/host/stubs -I$root_dir/include"

# shellcheck disable=SC2086
"$cc_bin" $common_cflags \
    "$root_dir/tests/host/runtime_macro_slots_test.c" \
    -o "$build_dir/runtime_macro_slots_test"

# shellcheck disable=SC2086
"$cc_bin" $common_cflags \
    "$root_dir/tests/host/runtime_macro_executor_test.c" \
    -o "$build_dir/runtime_macro_executor_test"

"$build_dir/runtime_macro_slots_test"
"$build_dir/runtime_macro_executor_test"
