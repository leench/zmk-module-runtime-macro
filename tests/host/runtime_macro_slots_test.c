/*
 * Host-side unit tests for the runtime macro slot store.
 *
 * The production source is included directly so the Settings handler can be
 * exercised without requiring a complete Zephyr build or a flash backend.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_LOG_LEVEL 0
#define CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT 2
#define CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN 8

#define TEST_MAX_TEXT_LEN CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN

static int save_result;
static int delete_result;
static unsigned int save_calls;
static unsigned int delete_calls;
static char saved_name[32];
static char saved_value[TEST_MAX_TEXT_LEN + 1];
static size_t saved_length;
static bool save_saw_updated_value;
static bool delete_saw_cleared_value;

enum concurrent_backend_operation {
    CONCURRENT_BACKEND_NONE,
    CONCURRENT_BACKEND_SAVE,
    CONCURRENT_BACKEND_DELETE,
};

static bool concurrent_backend_enabled;
static pthread_mutex_t concurrent_backend_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t concurrent_backend_condition = PTHREAD_COND_INITIALIZER;
static bool first_backend_entered;
static bool second_backend_entered;
static bool release_first_backend;
static unsigned int concurrent_backend_calls;
static enum concurrent_backend_operation concurrent_last_operation;
static char concurrent_last_value[TEST_MAX_TEXT_LEN + 1];
static size_t concurrent_last_length;

#include "../../src/runtime_macro_ascii.c"
#include "../../src/runtime_macro.c"

static int failures;

#define EXPECT_TRUE(condition)                                                                     \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            failures++;                                                                            \
        }                                                                                          \
    } while (false)

#define EXPECT_EQ(expected, actual)                                                                \
    do {                                                                                           \
        long long expected_value = (long long)(expected);                                          \
        long long actual_value = (long long)(actual);                                              \
        if (expected_value != actual_value) {                                                      \
            fprintf(stderr, "FAIL: %s:%d: expected %lld, got %lld\n", __FILE__, __LINE__,          \
                    expected_value, actual_value);                                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (false)

static void reset_backend(void) {
    save_result = 0;
    delete_result = 0;
    save_calls = 0;
    delete_calls = 0;
    saved_name[0] = '\0';
    saved_value[0] = '\0';
    saved_length = 0;
    save_saw_updated_value = false;
    delete_saw_cleared_value = false;
}

static void concurrent_backend_reset(void) {
    pthread_mutex_lock(&concurrent_backend_mutex);
    first_backend_entered = false;
    second_backend_entered = false;
    release_first_backend = false;
    concurrent_backend_calls = 0;
    concurrent_last_operation = CONCURRENT_BACKEND_NONE;
    concurrent_last_value[0] = '\0';
    concurrent_last_length = 0;
    pthread_mutex_unlock(&concurrent_backend_mutex);
    concurrent_backend_enabled = true;
}

static void concurrent_backend_record(enum concurrent_backend_operation operation,
                                      const void *value, size_t length) {
    concurrent_last_operation = operation;
    concurrent_last_length = length;
    if (length > 0) {
        memcpy(concurrent_last_value, value, length);
    }
    concurrent_last_value[length] = '\0';
}

static void concurrent_backend_enter(enum concurrent_backend_operation operation, const void *value,
                                     size_t length) {
    pthread_mutex_lock(&concurrent_backend_mutex);
    unsigned int call = concurrent_backend_calls++;
    if (call == 0) {
        first_backend_entered = true;
        pthread_cond_broadcast(&concurrent_backend_condition);
        while (!release_first_backend) {
            pthread_cond_wait(&concurrent_backend_condition, &concurrent_backend_mutex);
        }
    } else if (call == 1) {
        second_backend_entered = true;
        pthread_cond_broadcast(&concurrent_backend_condition);
    }

    concurrent_backend_record(operation, value, length);
    pthread_mutex_unlock(&concurrent_backend_mutex);
}

static bool wait_for_first_backend(void) {
    pthread_mutex_lock(&concurrent_backend_mutex);
    while (!first_backend_entered) {
        pthread_cond_wait(&concurrent_backend_condition, &concurrent_backend_mutex);
    }
    pthread_mutex_unlock(&concurrent_backend_mutex);
    return true;
}

static bool wait_for_second_backend(unsigned int timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&concurrent_backend_mutex);
    while (!second_backend_entered) {
        int err = pthread_cond_timedwait(&concurrent_backend_condition, &concurrent_backend_mutex,
                                         &deadline);
        if (err == ETIMEDOUT) {
            break;
        }
    }
    bool entered = second_backend_entered;
    pthread_mutex_unlock(&concurrent_backend_mutex);
    return entered;
}

static void release_first_backend_call(void) {
    pthread_mutex_lock(&concurrent_backend_mutex);
    release_first_backend = true;
    pthread_cond_broadcast(&concurrent_backend_condition);
    pthread_mutex_unlock(&concurrent_backend_mutex);
}

int settings_save_one(const char *name, const void *value, size_t length) {
    if (concurrent_backend_enabled) {
        (void)name;
        concurrent_backend_enter(CONCURRENT_BACKEND_SAVE, value, length);
        return 0;
    }
    const char *slot_text;

    save_calls++;
    snprintf(saved_name, sizeof(saved_name), "%s", name);
    saved_length = length;
    memcpy(saved_value, value, length);
    saved_value[length] = '\0';

    if (zmk_runtime_macro_slot_get(1, &slot_text) == 0 && strcmp(slot_text, "new") == 0) {
        save_saw_updated_value = true;
    }

    return save_result;
}

int settings_delete(const char *name) {
    if (concurrent_backend_enabled) {
        (void)name;
        concurrent_backend_enter(CONCURRENT_BACKEND_DELETE, NULL, 0);
        return 0;
    }

    const char *slot_text;

    delete_calls++;
    snprintf(saved_name, sizeof(saved_name), "%s", name);

    if (zmk_runtime_macro_slot_get(1, &slot_text) == 0 && strcmp(slot_text, "") == 0) {
        delete_saw_cleared_value = true;
    }

    return delete_result;
}

struct reader_context {
    const char *data;
    size_t copy_length;
    ssize_t result;
};

static ssize_t read_value(void *cb_arg, void *data, size_t length) {
    struct reader_context *context = cb_arg;

    if (context->copy_length > length) {
        return -EINVAL;
    }

    memcpy(data, context->data, context->copy_length);
    return context->result;
}

static void expect_slot(uint8_t slot, const char *expected, size_t expected_length) {
    const char *text;
    size_t length;

    EXPECT_EQ(0, zmk_runtime_macro_slot_get(slot, &text));
    EXPECT_TRUE(strcmp(text, expected) == 0);
    EXPECT_EQ(0, zmk_runtime_macro_slot_get_length(slot, &length));
    EXPECT_EQ(expected_length, length);
    EXPECT_EQ(0, text[expected_length]);
}

enum concurrent_update_kind {
    CONCURRENT_UPDATE_SET,
    CONCURRENT_UPDATE_CLEAR,
};

struct concurrent_update_args {
    enum concurrent_update_kind kind;
    const char *text;
    int result;
};

static void *run_concurrent_update(void *arg) {
    struct concurrent_update_args *update = arg;

    if (update->kind == CONCURRENT_UPDATE_SET) {
        update->result = zmk_runtime_macro_slot_set(1, update->text, strlen(update->text));
    } else {
        update->result = zmk_runtime_macro_slot_clear(1);
    }

    return NULL;
}

static void test_defaults_and_basic_api(void) {
    const char *text;
    size_t length;

    reset_backend();
    expect_slot(0, "", 0);
    expect_slot(1, "", 0);

    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_get(0, NULL));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_get_length(0, NULL));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_get(2, &text));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_get_length(2, &length));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_set(2, "x", 1));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_clear(2));
    EXPECT_EQ(0, save_calls);
    EXPECT_EQ(0, delete_calls);

    EXPECT_EQ(0, zmk_runtime_macro_slot_set(1, "Hi\n\t\b!", 6));
    EXPECT_EQ(1, save_calls);
    EXPECT_TRUE(strcmp(saved_name, "runtime_macro/slot/1") == 0);
    EXPECT_EQ(6, saved_length);
    EXPECT_TRUE(strcmp(saved_value, "Hi\n\t\b!") == 0);
    expect_slot(1, "Hi\n\t\b!", 6);

    EXPECT_EQ(0, zmk_runtime_macro_slot_clear(1));
    EXPECT_EQ(1, delete_calls);
    EXPECT_TRUE(strcmp(saved_name, "runtime_macro/slot/1") == 0);
    expect_slot(1, "", 0);

    EXPECT_EQ(0, zmk_runtime_macro_slot_set(1, NULL, 0));
    EXPECT_EQ(2, save_calls);
    expect_slot(1, "", 0);
}

static void test_length_and_character_validation(void) {
    char maximum[TEST_MAX_TEXT_LEN];
    char too_long[TEST_MAX_TEXT_LEN + 1];
    const char embedded_nul[] = {'a', '\0', 'b'};
    const char *text;
    size_t length;

    reset_backend();
    memset(maximum, 'x', sizeof(maximum));
    memset(too_long, 'y', sizeof(too_long));

    EXPECT_EQ(0, zmk_runtime_macro_slot_set(0, maximum, sizeof(maximum)));
    expect_slot(0, "xxxxxxxx", TEST_MAX_TEXT_LEN);
    EXPECT_EQ(1, save_calls);

    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_set(0, too_long, sizeof(too_long)));
    EXPECT_EQ(1, save_calls);
    expect_slot(0, "xxxxxxxx", TEST_MAX_TEXT_LEN);

    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_set(0, "bad\r", 4));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_set(0, "bad\177", 4));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_set(0, embedded_nul, sizeof(embedded_nul)));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_slot_set(0, "\303\274", 2));
    EXPECT_EQ(1, save_calls);
    expect_slot(0, "xxxxxxxx", TEST_MAX_TEXT_LEN);

    EXPECT_EQ(0, zmk_runtime_macro_slot_get(0, &text));
    EXPECT_EQ(0, zmk_runtime_macro_slot_get_length(0, &length));
    EXPECT_EQ(0, text[length]);
}

static void test_persistence_error_ordering(void) {
    reset_backend();
    save_result = -EIO;
    EXPECT_EQ(-EIO, zmk_runtime_macro_slot_set(1, "new", 3));
    EXPECT_TRUE(save_saw_updated_value);
    expect_slot(1, "new", 3);

    delete_result = -ENOSPC;
    EXPECT_EQ(-ENOSPC, zmk_runtime_macro_slot_clear(1));
    EXPECT_TRUE(delete_saw_cleared_value);
    expect_slot(1, "", 0);
}

static void test_settings_key_parser_and_load(void) {
    struct reader_context context = {
        .data = "loaded",
        .copy_length = 6,
        .result = 6,
    };

    reset_backend();
    EXPECT_EQ(0, zmk_runtime_macro_slot_set(0, "keep", 4));
    EXPECT_EQ(0, runtime_macro_settings_set("slot/1", 6, read_value, &context));
    expect_slot(1, "loaded", 6);

    EXPECT_EQ(-ENOENT, runtime_macro_settings_set("other/1", 1, read_value, &context));
    EXPECT_EQ(-ENOENT, runtime_macro_settings_set(NULL, 1, read_value, &context));
    EXPECT_EQ(-ENOENT, runtime_macro_settings_set("slotx/1", 1, read_value, &context));
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot", 1, read_value, &context));
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/", 0, read_value, &context));
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/0/extra", 1, read_value, &context));
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/a", 1, read_value, &context));
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/2", 1, read_value, &context));
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/02", 1, read_value, &context));
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/0=", 1, read_value, &context));
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/4294967296", 1, read_value, &context));
    EXPECT_EQ(-EINVAL,
              runtime_macro_settings_set("slot/999999999999999999999999", 1, read_value, &context));
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/0", 1, NULL, &context));
}

static void test_concurrent_persistence_order(enum concurrent_update_kind second_kind) {
    pthread_t first_thread;
    pthread_t second_thread;
    struct concurrent_update_args first = {
        .kind = CONCURRENT_UPDATE_SET,
        .text = "first",
        .result = -1,
    };
    struct concurrent_update_args second = {
        .kind = second_kind,
        .text = "second",
        .result = -1,
    };

    reset_backend();
    EXPECT_EQ(0, zmk_runtime_macro_slot_set(1, "initial", 7));
    concurrent_backend_reset();

    int err = pthread_create(&first_thread, NULL, run_concurrent_update, &first);
    EXPECT_EQ(0, err);
    if (err != 0) {
        concurrent_backend_enabled = false;
        return;
    }

    EXPECT_TRUE(wait_for_first_backend());
    err = pthread_create(&second_thread, NULL, run_concurrent_update, &second);
    EXPECT_EQ(0, err);
    if (err != 0) {
        release_first_backend_call();
        pthread_join(first_thread, NULL);
        concurrent_backend_enabled = false;
        return;
    }

    /*
     * The first backend call is held open. With the update mutex, the second
     * operation cannot reach its backend call until the first one returns.
     * Without it, the second call enters here and completes first, allowing
     * the first call to overwrite the persisted value afterwards.
     */
    bool second_entered_before_release = wait_for_second_backend(500);
    release_first_backend_call();
    EXPECT_EQ(0, pthread_join(first_thread, NULL));
    EXPECT_EQ(0, pthread_join(second_thread, NULL));
    concurrent_backend_enabled = false;

    EXPECT_EQ(0, first.result);
    EXPECT_EQ(0, second.result);
    EXPECT_TRUE(!second_entered_before_release);
    EXPECT_EQ(2, concurrent_backend_calls);

    const char *slot_text;
    EXPECT_EQ(0, zmk_runtime_macro_slot_get(1, &slot_text));
    if (second_kind == CONCURRENT_UPDATE_SET) {
        EXPECT_TRUE(strcmp(slot_text, "second") == 0);
        EXPECT_EQ(CONCURRENT_BACKEND_SAVE, concurrent_last_operation);
        EXPECT_EQ(6, concurrent_last_length);
        EXPECT_TRUE(strcmp(concurrent_last_value, "second") == 0);
    } else {
        EXPECT_TRUE(strcmp(slot_text, "") == 0);
        EXPECT_EQ(CONCURRENT_BACKEND_DELETE, concurrent_last_operation);
        EXPECT_EQ(0, concurrent_last_length);
        EXPECT_TRUE(strcmp(concurrent_last_value, "") == 0);
    }
}

static void test_settings_read_errors_do_not_mutate(void) {
    struct reader_context error_context = {
        .data = "ignored",
        .copy_length = 0,
        .result = -EIO,
    };
    struct reader_context incomplete_context = {
        .data = "part",
        .copy_length = 4,
        .result = 3,
    };
    struct reader_context invalid_value_context = {
        .data = "bad\r",
        .copy_length = 4,
        .result = 4,
    };

    reset_backend();
    EXPECT_EQ(0, zmk_runtime_macro_slot_set(0, "keep", 4));
    EXPECT_EQ(-EIO, runtime_macro_settings_set("slot/0", 4, read_value, &error_context));
    expect_slot(0, "keep", 4);
    EXPECT_EQ(-EIO, runtime_macro_settings_set("slot/0", 4, read_value, &incomplete_context));
    expect_slot(0, "keep", 4);
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/0", 4, read_value, &invalid_value_context));
    expect_slot(0, "keep", 4);
    EXPECT_EQ(-EINVAL, runtime_macro_settings_set("slot/0", TEST_MAX_TEXT_LEN + 1, read_value,
                                                  &error_context));
    expect_slot(0, "keep", 4);

    struct reader_context empty_context = {
        .data = "",
        .copy_length = 0,
        .result = 0,
    };
    EXPECT_EQ(0, runtime_macro_settings_set("slot/0", 0, read_value, &empty_context));
    expect_slot(0, "", 0);
}

int main(void) {
    test_defaults_and_basic_api();
    test_length_and_character_validation();
    test_persistence_error_ordering();
    test_concurrent_persistence_order(CONCURRENT_UPDATE_SET);
    test_concurrent_persistence_order(CONCURRENT_UPDATE_CLEAR);
    test_settings_key_parser_and_load();
    test_settings_read_errors_do_not_mutate();

    if (failures != 0) {
        fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return 1;
    }

    puts("runtime macro slot tests: PASS");
    return 0;
}
