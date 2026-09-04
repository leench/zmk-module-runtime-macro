#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);

struct settings_handler {
    const char *name;
    int (*h_get)(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg);
    int (*h_set)(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg);
    int (*h_commit)(void);
    int (*h_export)(int (*storage_func)(const char *name, const void *value, size_t val_len));
};

extern int settings_save_one(const char *name, const void *value, size_t val_len);
extern int settings_delete(const char *name);

static inline bool settings_name_steq(const char *name, const char *key, const char **next) {
    if (next != NULL) {
        *next = NULL;
    }

    if (name == NULL || key == NULL) {
        return false;
    }

    while (*key != '\0' && *name != '\0' && *name != '=' && *key == *name) {
        key++;
        name++;
    }

    if (*key != '\0') {
        return false;
    }

    if (*name == '/') {
        if (next != NULL) {
            *next = name + 1;
        }
        return true;
    }

    return *name == '\0' || *name == '=';
}

#define SETTINGS_STATIC_HANDLER_DEFINE(_name, _subtree, _h_get, _h_set, _h_commit, _h_export)      \
    static const struct settings_handler __attribute__((unused)) settings_handler_##_name = {      \
        .name = (_subtree),                                                                        \
        .h_get = (_h_get),                                                                         \
        .h_set = (_h_set),                                                                         \
        .h_commit = (_h_commit),                                                                   \
        .h_export = (_h_export),                                                                   \
    }

#ifdef __cplusplus
}
#endif
