/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Start the shared runtime macro executor from already validated bytes.
 *
 * The executor copies the input into its own snapshot before scheduling work.
 * Callers retain ownership of the input. The snapshot is consumed only after
 * the work item is accepted; busy and scheduling errors leave the input alone.
 *
 * @param text Validated macro bytes. May be NULL only when length is zero.
 * @param length Number of bytes, excluding the terminator.
 * @retval 0 If the snapshot was accepted (or length is zero).
 * @retval -EINVAL If the input or length is invalid.
 * @retval -EBUSY If another macro is already executing.
 * @retval Negative errno If the executor work item cannot be scheduled.
 */
int zmk_runtime_macro_executor_start(const uint8_t *text, size_t length);
