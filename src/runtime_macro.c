/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/runtime_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static char runtime_macro_slots[CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT]
                               [CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN + 1];
static size_t runtime_macro_slot_lengths[CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT];
static K_MUTEX_DEFINE(runtime_macro_slots_mutex);
static K_MUTEX_DEFINE(runtime_macro_slot_update_mutex);

#define RUNTIME_MACRO_DEFAULTS_SETTING_NAME "defaults_initialized"
#define RUNTIME_MACRO_DEFAULTS_SETTING_PATH \
    "runtime_macro/" RUNTIME_MACRO_DEFAULTS_SETTING_NAME
#define RUNTIME_MACRO_DEFAULTS_MARKER 1U

static bool runtime_macro_slot_has_persisted_setting[CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT];
static bool runtime_macro_defaults_initialized;

static bool runtime_macro_slot_is_valid(uint8_t slot) {
    return slot < CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT;
}

static bool runtime_macro_text_is_valid(const char *text, size_t length) {
    if (length > 0 && text == NULL) {
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        uint32_t encoded;

        if (zmk_runtime_macro_ascii_to_keycode((uint8_t)text[i], &encoded) == 0) {
            continue;
        }

        return false;
    }

    return true;
}

static void runtime_macro_slot_setting_name(uint8_t slot, char *name, size_t name_size) {
    snprintf(name, name_size, "runtime_macro/slot/%u", slot);
}

static size_t runtime_macro_default_text(uint8_t slot, char *text, size_t text_size) {
    if (text == NULL || text_size == 0U) {
        return 0U;
    }

    int written = snprintf(text, text_size, "Runtime Macro %u", (unsigned int)slot + 1U);
    if (written < 0) {
        text[0] = '\0';
        return 0U;
    }

    if ((size_t)written >= text_size) {
        return text_size - 1U;
    }

    return (size_t)written;
}

static void runtime_macro_slot_replace(uint8_t slot, const char *text, size_t length) {
    if (length > 0) {
        memmove(runtime_macro_slots[slot], text, length);
    }

    runtime_macro_slots[slot][length] = '\0';
    runtime_macro_slot_lengths[slot] = length;
}

int zmk_runtime_macro_slot_get(uint8_t slot, const char **text) {
    if (!runtime_macro_slot_is_valid(slot) || text == NULL) {
        return -EINVAL;
    }

    *text = runtime_macro_slots[slot];
    return 0;
}

int zmk_runtime_macro_slot_get_length(uint8_t slot, size_t *length) {
    if (!runtime_macro_slot_is_valid(slot) || length == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&runtime_macro_slots_mutex, K_FOREVER);
    *length = runtime_macro_slot_lengths[slot];
    k_mutex_unlock(&runtime_macro_slots_mutex);
    return 0;
}

int zmk_runtime_macro_slot_copy(uint8_t slot, char *text, size_t text_size, size_t *length) {
    if (!runtime_macro_slot_is_valid(slot) || text == NULL || length == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&runtime_macro_slots_mutex, K_FOREVER);
    size_t slot_length = runtime_macro_slot_lengths[slot];
    if (text_size <= slot_length) {
        k_mutex_unlock(&runtime_macro_slots_mutex);
        return -ENOSPC;
    }

    memcpy(text, runtime_macro_slots[slot], slot_length + 1);
    *length = slot_length;
    k_mutex_unlock(&runtime_macro_slots_mutex);
    return 0;
}

int zmk_runtime_macro_slot_set(uint8_t slot, const char *text, size_t length) {
    if (!runtime_macro_slot_is_valid(slot) || length > CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN ||
        !runtime_macro_text_is_valid(text, length)) {
        return -EINVAL;
    }

    char persisted_value[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN + 1];
    k_mutex_lock(&runtime_macro_slot_update_mutex, K_FOREVER);

    k_mutex_lock(&runtime_macro_slots_mutex, K_FOREVER);
    runtime_macro_slot_replace(slot, text, length);
    memcpy(persisted_value, runtime_macro_slots[slot], length);
    k_mutex_unlock(&runtime_macro_slots_mutex);

    char setting_name[sizeof("runtime_macro/slot/") + 3];
    runtime_macro_slot_setting_name(slot, setting_name, sizeof(setting_name));

    int err = settings_save_one(setting_name, persisted_value, length);
    if (err != 0) {
        LOG_ERR("Failed to save runtime macro slot %u (err %d)", slot, err);
    }

    k_mutex_unlock(&runtime_macro_slot_update_mutex);
    return err;
}

int zmk_runtime_macro_slot_clear(uint8_t slot) {
    if (!runtime_macro_slot_is_valid(slot)) {
        return -EINVAL;
    }

    k_mutex_lock(&runtime_macro_slot_update_mutex, K_FOREVER);

    k_mutex_lock(&runtime_macro_slots_mutex, K_FOREVER);
    memset(runtime_macro_slots[slot], 0, sizeof(runtime_macro_slots[slot]));
    runtime_macro_slot_lengths[slot] = 0;
    k_mutex_unlock(&runtime_macro_slots_mutex);

    char setting_name[sizeof("runtime_macro/slot/") + 3];
    runtime_macro_slot_setting_name(slot, setting_name, sizeof(setting_name));

    int err = settings_delete(setting_name);
    if (err != 0) {
        LOG_ERR("Failed to delete runtime macro slot %u (err %d)", slot, err);
    }

    k_mutex_unlock(&runtime_macro_slot_update_mutex);
    return err;
}

static int runtime_macro_parse_slot(const char *name, uint8_t *slot) {
    if (name == NULL || *name == '\0' || slot == NULL) {
        return -EINVAL;
    }

    if (name[0] == '0' && name[1] != '\0') {
        return -EINVAL;
    }

    uint32_t value = 0;
    for (const char *cursor = name; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return -EINVAL;
        }

        uint32_t digit = (uint32_t)(*cursor - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return -EINVAL;
        }

        value = value * 10U + digit;
        if (value >= CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT) {
            return -EINVAL;
        }
    }

    *slot = (uint8_t)value;
    return 0;
}

static int runtime_macro_settings_set(const char *name, size_t length, settings_read_cb read_cb,
                                      void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, RUNTIME_MACRO_DEFAULTS_SETTING_NAME, &next)) {
        if (next != NULL || length != sizeof(uint8_t) || read_cb == NULL) {
            return -EINVAL;
        }

        uint8_t marker;
        ssize_t read_length = read_cb(cb_arg, &marker, sizeof(marker));
        if (read_length < 0) {
            return (int)read_length;
        }
        if ((size_t)read_length != sizeof(marker) || marker != RUNTIME_MACRO_DEFAULTS_MARKER) {
            return -EINVAL;
        }

        runtime_macro_defaults_initialized = true;
        return 0;
    }

    if (name == NULL || !settings_name_steq(name, "slot", &next)) {
        return -ENOENT;
    }

    uint8_t slot;
    int err = runtime_macro_parse_slot(next, &slot);
    if (err != 0) {
        return err;
    }

    /* Remember the key even when its value is malformed, so defaults do not
     * hide a persisted settings error. */
    runtime_macro_slot_has_persisted_setting[slot] = true;

    if (length > CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN || read_cb == NULL) {
        return -EINVAL;
    }

    char value[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN + 1];
    ssize_t read_length = read_cb(cb_arg, value, length);
    if (read_length < 0) {
        return (int)read_length;
    }

    if ((size_t)read_length != length) {
        return -EIO;
    }

    if (!runtime_macro_text_is_valid(value, length)) {
        return -EINVAL;
    }

    k_mutex_lock(&runtime_macro_slots_mutex, K_FOREVER);
    runtime_macro_slot_replace(slot, value, length);
    k_mutex_unlock(&runtime_macro_slots_mutex);
    return 0;
}

static int runtime_macro_settings_commit(void) {
    if (runtime_macro_defaults_initialized) {
        return 0;
    }

    bool defaults_persisted = true;
    for (uint8_t slot = 0; slot < CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT; slot++) {
        if (runtime_macro_slot_has_persisted_setting[slot]) {
            continue;
        }

        char default_text[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN + 1];
        size_t default_length =
            runtime_macro_default_text(slot, default_text, sizeof(default_text));

        k_mutex_lock(&runtime_macro_slots_mutex, K_FOREVER);
        runtime_macro_slot_replace(slot, default_text, default_length);
        k_mutex_unlock(&runtime_macro_slots_mutex);

        char setting_name[sizeof("runtime_macro/slot/") + 3];
        runtime_macro_slot_setting_name(slot, setting_name, sizeof(setting_name));

        int err = settings_save_one(setting_name, default_text, default_length);
        if (err != 0) {
            defaults_persisted = false;
            LOG_ERR("Failed to save default runtime macro slot %u (err %d)", slot, err);
        } else {
            runtime_macro_slot_has_persisted_setting[slot] = true;
        }
    }

    if (!defaults_persisted) {
        return 0;
    }

    const uint8_t marker = RUNTIME_MACRO_DEFAULTS_MARKER;
    int err = settings_save_one(RUNTIME_MACRO_DEFAULTS_SETTING_PATH, &marker, sizeof(marker));
    if (err != 0) {
        LOG_ERR("Failed to save runtime macro defaults marker (err %d)", err);
        return 0;
    }

    runtime_macro_defaults_initialized = true;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(runtime_macro, "runtime_macro", NULL, runtime_macro_settings_set,
                               runtime_macro_settings_commit, NULL);
