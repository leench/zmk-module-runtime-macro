/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Convert one US ASCII byte to a ZMK encoded keyboard keycode.
 *
 * Printable ASCII bytes 0x20 through 0x7e and the controls '\n', '\t', and
 * '\b' are supported. Shifted characters include MOD_LSFT in the encoded
 * keycode. The mapping is also used to validate slot contents.
 *
 * @param byte ASCII byte to convert.
 * @param encoded Receives the ZMK encoded keycode.
 * @retval 0 If the byte is supported and encoded is non-NULL.
 * @retval -EINVAL If encoded is NULL or the byte is unsupported.
 */
int zmk_runtime_macro_ascii_to_keycode(uint8_t byte, uint32_t *encoded);

/**
 * @brief Get the text stored in a runtime macro slot.
 *
 * The returned string is owned by the runtime macro module. Callers must not
 * free or modify it. The pointer remains valid until the same slot is changed
 * by zmk_runtime_macro_slot_set() or zmk_runtime_macro_slot_clear(). Use
 * zmk_runtime_macro_slot_copy() when a caller needs a synchronized snapshot.
 *
 * @param slot Slot index.
 * @param text Receives a pointer to the NUL-terminated slot text.
 * @retval 0 If the slot and output pointer are valid.
 * @retval -EINVAL If slot is out of range or text is NULL.
 */
int zmk_runtime_macro_slot_get(uint8_t slot, const char **text);

/**
 * @brief Get the length of the text stored in a runtime macro slot.
 *
 * The length excludes the terminating NUL byte.
 *
 * @param slot Slot index.
 * @param length Receives the slot text length.
 * @retval 0 If the slot and output pointer are valid.
 * @retval -EINVAL If slot is out of range or length is NULL.
 */
int zmk_runtime_macro_slot_get_length(uint8_t slot, size_t *length);

/**
 * @brief Copy a consistent snapshot of a runtime macro slot.
 *
 * Unlike zmk_runtime_macro_slot_get(), this API keeps the slot lock while
 * copying, so the returned buffer is independent from subsequent slot writes.
 *
 * @param slot Slot index.
 * @param text Destination buffer for the NUL-terminated text.
 * @param text_size Destination buffer size in bytes.
 * @param length Receives the text length excluding the terminating NUL.
 * @retval 0 If the snapshot was copied.
 * @retval -EINVAL If an argument is invalid.
 * @retval -ENOSPC If text_size cannot hold the slot text and NUL terminator.
 */
int zmk_runtime_macro_slot_copy(uint8_t slot, char *text, size_t text_size, size_t *length);

/**
 * @brief Start executing a runtime macro slot.
 *
 * The slot is copied before execution begins. A second invocation while an
 * existing macro is running returns -EBUSY and is not queued.
 *
 * @param slot Slot index.
 * @retval 0 If the macro was started or the slot was empty.
 * @retval -EINVAL If the slot is invalid or the slot snapshot fails.
 * @retval -EBUSY If another runtime macro is running.
 * @retval Negative errno If the executor cannot schedule its work item.
 */
int zmk_runtime_macro_execute(uint8_t slot);

/**
 * @brief Return whether a runtime macro is currently executing.
 */
bool zmk_runtime_macro_is_busy(void);

/**
 * @brief Replace a runtime macro slot and persist the new text.
 *
 * Only printable US ASCII bytes and the controls '\n', '\t', and '\b' are
 * accepted. The RAM value is updated before the Settings backend is called;
 * therefore a persistence error does not roll back the in-memory value.
 *
 * @param slot Slot index.
 * @param text Text bytes to store. May be NULL only when length is zero.
 * @param length Number of text bytes, excluding the terminating NUL.
 * @retval 0 If the value was updated and persisted.
 * @retval -EINVAL If the slot, text, length, or a byte is invalid.
 * @retval Negative errno If the Settings backend fails.
 */
int zmk_runtime_macro_slot_set(uint8_t slot, const char *text, size_t length);

/**
 * @brief Clear a runtime macro slot and remove its persisted value.
 *
 * The RAM value is cleared before the Settings backend is called; therefore a
 * deletion error does not roll back the in-memory value.
 *
 * @param slot Slot index.
 * @retval 0 If the slot was cleared and its persisted value was deleted.
 * @retval -EINVAL If slot is out of range.
 * @retval Negative errno If the Settings backend fails.
 */
int zmk_runtime_macro_slot_clear(uint8_t slot);
