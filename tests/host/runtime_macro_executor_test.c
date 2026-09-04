/*
 * Host-side tests for ASCII conversion and the runtime macro executor.
 *
 * The production sources are included directly so the delayable work state
 * machine can be driven deterministically without a Zephyr scheduler.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>

#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_LOG_LEVEL 0
#define CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT 2
#define CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN 8
#define CONFIG_ZMK_MACRO_DEFAULT_TAP_MS 10
#define CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS 20

static int save_result;
static int delete_result;
static int raise_error_call = -1;
static int schedule_failure_call = -1;
static int raise_calls;
static int schedule_calls;
static unsigned int event_count;
static int64_t host_uptime;

struct captured_event {
    uint32_t encoded;
    bool pressed;
    int64_t timestamp;
};

static struct captured_event events[32];

#include "../../src/runtime_macro_ascii.c"
#include "../../src/runtime_macro.c"
#include "../../src/runtime_macro_executor.c"

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

int settings_save_one(const char *name, const void *value, size_t length) {
    (void)name;
    (void)value;
    (void)length;
    return save_result;
}

int settings_delete(const char *name) {
    (void)name;
    return delete_result;
}

int host_work_schedule(struct k_work_delayable *work, k_timeout_t delay, bool reschedule) {
    (void)reschedule;
    int call = schedule_calls++;
    if (call == schedule_failure_call) {
        return -EIO;
    }

    work->scheduled = true;
    work->delay = delay;
    return 1;
}

int raise_zmk_keycode_state_changed_from_encoded(uint32_t encoded, bool pressed,
                                                 int64_t timestamp) {
    if (event_count < sizeof(events) / sizeof(events[0])) {
        events[event_count++] = (struct captured_event){
            .encoded = encoded,
            .pressed = pressed,
            .timestamp = timestamp,
        };
    }

    int call = raise_calls++;
    if (call == raise_error_call) {
        return -EIO;
    }

    return 0;
}

static void reset_test_state(void) {
    save_result = 0;
    delete_result = 0;
    raise_error_call = -1;
    schedule_failure_call = -1;
    raise_calls = 0;
    schedule_calls = 0;
    event_count = 0;
    host_uptime = 1000;
    memset(events, 0, sizeof(events));
    runtime_macro_executor_work.scheduled = false;
    atomic_set(&runtime_macro_executor_busy, 0);
}

static uint32_t expected_keycode(uint8_t usage, bool shifted) {
    return ZMK_HID_USAGE(HID_USAGE_KEY, usage) | (shifted ? ((uint32_t)MOD_LSFT << 24) : 0);
}

static uint8_t expected_usage(uint8_t byte) {
    if (byte >= 'a' && byte <= 'z') {
        return HID_USAGE_KEY_KEYBOARD_A + byte - 'a';
    }
    if (byte >= 'A' && byte <= 'Z') {
        return HID_USAGE_KEY_KEYBOARD_A + byte - 'A';
    }
    if (byte >= '1' && byte <= '9') {
        return HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION + byte - '1';
    }
    if (byte == '0') {
        return HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS;
    }

    switch (byte) {
    case '\b':
        return HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE;
    case '\t':
        return HID_USAGE_KEY_KEYBOARD_TAB;
    case '\n':
        return HID_USAGE_KEY_KEYBOARD_RETURN_ENTER;
    case ' ':
        return HID_USAGE_KEY_KEYBOARD_SPACEBAR;
    case '!':
        return HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION;
    case '"':
        return HID_USAGE_KEY_KEYBOARD_APOSTROPHE_AND_QUOTE;
    case '#':
        return HID_USAGE_KEY_KEYBOARD_3_AND_HASH;
    case '$':
        return HID_USAGE_KEY_KEYBOARD_4_AND_DOLLAR;
    case '%':
        return HID_USAGE_KEY_KEYBOARD_5_AND_PERCENT;
    case '&':
        return HID_USAGE_KEY_KEYBOARD_7_AND_AMPERSAND;
    case '\'':
        return HID_USAGE_KEY_KEYBOARD_APOSTROPHE_AND_QUOTE;
    case '(':
        return HID_USAGE_KEY_KEYBOARD_9_AND_LEFT_PARENTHESIS;
    case ')':
        return HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS;
    case '*':
        return HID_USAGE_KEY_KEYBOARD_8_AND_ASTERISK;
    case '+':
        return HID_USAGE_KEY_KEYBOARD_EQUAL_AND_PLUS;
    case ',':
        return HID_USAGE_KEY_KEYBOARD_COMMA_AND_LESS_THAN;
    case '-':
        return HID_USAGE_KEY_KEYBOARD_MINUS_AND_UNDERSCORE;
    case '.':
        return HID_USAGE_KEY_KEYBOARD_PERIOD_AND_GREATER_THAN;
    case '/':
        return HID_USAGE_KEY_KEYBOARD_SLASH_AND_QUESTION_MARK;
    case ':':
    case ';':
        return HID_USAGE_KEY_KEYBOARD_SEMICOLON_AND_COLON;
    case '<':
        return HID_USAGE_KEY_KEYBOARD_COMMA_AND_LESS_THAN;
    case '=':
        return HID_USAGE_KEY_KEYBOARD_EQUAL_AND_PLUS;
    case '>':
        return HID_USAGE_KEY_KEYBOARD_PERIOD_AND_GREATER_THAN;
    case '?':
        return HID_USAGE_KEY_KEYBOARD_SLASH_AND_QUESTION_MARK;
    case '@':
        return HID_USAGE_KEY_KEYBOARD_2_AND_AT;
    case '[':
    case '{':
        return HID_USAGE_KEY_KEYBOARD_LEFT_BRACKET_AND_LEFT_BRACE;
    case '\\':
    case '|':
        return HID_USAGE_KEY_KEYBOARD_BACKSLASH_AND_PIPE;
    case ']':
    case '}':
        return HID_USAGE_KEY_KEYBOARD_RIGHT_BRACKET_AND_RIGHT_BRACE;
    case '^':
        return HID_USAGE_KEY_KEYBOARD_6_AND_CARET;
    case '_':
        return HID_USAGE_KEY_KEYBOARD_MINUS_AND_UNDERSCORE;
    case '`':
    case '~':
        return HID_USAGE_KEY_KEYBOARD_GRAVE_ACCENT_AND_TILDE;
    default:
        return 0;
    }
}

static bool expected_shift(uint8_t byte) {
    if (byte >= 'A' && byte <= 'Z') {
        return true;
    }

    switch (byte) {
    case '!':
    case '"':
    case '#':
    case '$':
    case '%':
    case '&':
    case '(':
    case ')':
    case '*':
    case '+':
    case ':':
    case '<':
    case '>':
    case '?':
    case '@':
    case '^':
    case '_':
    case '{':
    case '|':
    case '}':
    case '~':
        return true;
    default:
        return false;
    }
}

static void test_ascii_mapping(void) {
    for (unsigned int byte = 0x20; byte <= 0x7e; byte++) {
        uint32_t encoded;
        EXPECT_EQ(0, zmk_runtime_macro_ascii_to_keycode((uint8_t)byte, &encoded));
        EXPECT_EQ(expected_keycode(expected_usage((uint8_t)byte), expected_shift((uint8_t)byte)),
                  encoded);
    }

    for (unsigned int byte = 0; byte < 0x20; byte++) {
        uint32_t encoded;
        int err = zmk_runtime_macro_ascii_to_keycode((uint8_t)byte, &encoded);
        if (byte == '\b' || byte == '\t' || byte == '\n') {
            EXPECT_EQ(0, err);
        } else {
            EXPECT_EQ(-EINVAL, err);
        }
    }

    uint32_t encoded;
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_ascii_to_keycode(0x7f, &encoded));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_ascii_to_keycode(0x80, &encoded));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_ascii_to_keycode(0xff, &encoded));
    EXPECT_EQ(-EINVAL, zmk_runtime_macro_ascii_to_keycode('a', NULL));
}

static void set_slot(uint8_t slot, const char *text) {
    EXPECT_EQ(0, zmk_runtime_macro_slot_set(slot, text, strlen(text)));
}

static void run_work_once(k_timeout_t expected_delay) {
    EXPECT_TRUE(runtime_macro_executor_work.scheduled);
    EXPECT_EQ(expected_delay, runtime_macro_executor_work.delay);
    runtime_macro_executor_work.scheduled = false;
    runtime_macro_executor_work.work.handler(&runtime_macro_executor_work.work);
}

static void expect_event(unsigned int index, uint8_t byte, bool pressed, int64_t timestamp) {
    uint32_t encoded;
    EXPECT_EQ(0, zmk_runtime_macro_ascii_to_keycode(byte, &encoded));
    EXPECT_TRUE(index < event_count);
    if (index < event_count) {
        EXPECT_EQ(encoded, events[index].encoded);
        EXPECT_EQ(pressed, events[index].pressed);
        EXPECT_EQ(timestamp, events[index].timestamp);
    }
}

static void expect_fixed_event(unsigned int index, uint8_t usage, bool pressed, int64_t timestamp) {
    uint32_t encoded = ZMK_HID_USAGE(HID_USAGE_KEY, usage);

    EXPECT_TRUE(index < event_count);
    if (index < event_count) {
        EXPECT_EQ(encoded, events[index].encoded);
        EXPECT_EQ(0, events[index].encoded & (0xffu << 24));
        EXPECT_EQ(pressed, events[index].pressed);
        EXPECT_EQ(timestamp, events[index].timestamp);
    }
}

static void test_executor_sequence_and_snapshot(void) {
    reset_test_state();
    set_slot(0, "aA1!");
    set_slot(1, "q");

    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    EXPECT_TRUE(zmk_runtime_macro_is_busy());
    EXPECT_EQ(0, event_count);
    run_work_once(K_NO_WAIT);
    expect_event(0, 'a', true, 1000);

    host_uptime = 1001;
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    expect_event(1, 'a', false, 1001);

    host_uptime = 1002;
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS);
    expect_event(2, 'A', true, 1002);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    expect_event(3, 'A', false, 1002);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS);
    expect_event(4, '1', true, 1002);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    expect_event(5, '1', false, 1002);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS);
    expect_event(6, '!', true, 1002);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    expect_event(7, '!', false, 1002);

    EXPECT_EQ(8, event_count);
    EXPECT_TRUE(!runtime_macro_executor_work.scheduled);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());

    reset_test_state();
    set_slot(0, "ab");
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    set_slot(0, "Z");
    EXPECT_EQ(-EBUSY, zmk_runtime_macro_execute(1));
    run_work_once(K_NO_WAIT);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    EXPECT_EQ(4, event_count);
    expect_event(0, 'a', true, 1000);
    expect_event(1, 'a', false, 1000);
    expect_event(2, 'b', true, 1000);
    expect_event(3, 'b', false, 1000);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());
}

static void test_controls_and_maximum_length(void) {
    reset_test_state();
    set_slot(0, "\n\t\b");
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    run_work_once(K_NO_WAIT);
    expect_fixed_event(0, HID_USAGE_KEY_KEYBOARD_RETURN_ENTER, true, 1000);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    expect_fixed_event(1, HID_USAGE_KEY_KEYBOARD_RETURN_ENTER, false, 1000);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS);
    expect_fixed_event(2, HID_USAGE_KEY_KEYBOARD_TAB, true, 1000);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    expect_fixed_event(3, HID_USAGE_KEY_KEYBOARD_TAB, false, 1000);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS);
    expect_fixed_event(4, HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE, true, 1000);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    expect_fixed_event(5, HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE, false, 1000);
    EXPECT_EQ(6, event_count);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());

    reset_test_state();
    const char maximum[] = "abcdefgh";
    set_slot(0, maximum);
    EXPECT_EQ(CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN, strlen(maximum));
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    for (size_t i = 0; i < CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN; i++) {
        run_work_once(i == 0 ? K_NO_WAIT : CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS);
        expect_event((unsigned int)(i * 2), (uint8_t)maximum[i], true, 1000);
        run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
        expect_event((unsigned int)(i * 2 + 1), (uint8_t)maximum[i], false, 1000);
    }
    EXPECT_EQ(CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN * 2, event_count);
    EXPECT_TRUE(!runtime_macro_executor_work.scheduled);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());
}

static void test_empty_slot_and_busy_recovery(void) {
    reset_test_state();
    set_slot(0, "");
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    EXPECT_EQ(0, event_count);
    EXPECT_EQ(0, schedule_calls);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());

    set_slot(0, "x");
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    EXPECT_EQ(-EBUSY, zmk_runtime_macro_execute(0));
    run_work_once(K_NO_WAIT);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());

    reset_test_state();
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    run_work_once(K_NO_WAIT);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    EXPECT_EQ(2, event_count);
}

static void test_event_errors_and_schedule_failure(void) {
    reset_test_state();
    set_slot(0, "x");
    schedule_failure_call = 0;
    EXPECT_EQ(-EIO, zmk_runtime_macro_execute(0));
    EXPECT_EQ(1, schedule_calls);
    EXPECT_EQ(0, event_count);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());

    reset_test_state();
    set_slot(0, "x");
    raise_error_call = 0;
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    run_work_once(K_NO_WAIT);
    EXPECT_TRUE(zmk_runtime_macro_is_busy());
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    EXPECT_EQ(2, event_count);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());

    reset_test_state();
    set_slot(0, "xy");
    raise_error_call = 1;
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    run_work_once(K_NO_WAIT);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    EXPECT_EQ(4, event_count);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());

    reset_test_state();
    set_slot(0, "x");
    schedule_failure_call = 1;
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    run_work_once(K_NO_WAIT);
    EXPECT_EQ(2, event_count);
    EXPECT_TRUE(!runtime_macro_executor_work.scheduled);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());

    reset_test_state();
    set_slot(0, "xy");
    schedule_failure_call = 2;
    EXPECT_EQ(0, zmk_runtime_macro_execute(0));
    run_work_once(K_NO_WAIT);
    run_work_once(CONFIG_ZMK_MACRO_DEFAULT_TAP_MS);
    EXPECT_EQ(2, event_count);
    expect_event(0, 'x', true, 1000);
    expect_event(1, 'x', false, 1000);
    EXPECT_TRUE(!runtime_macro_executor_work.scheduled);
    EXPECT_TRUE(!zmk_runtime_macro_is_busy());
}

int main(void) {
    test_ascii_mapping();
    test_executor_sequence_and_snapshot();
    test_controls_and_maximum_length();
    test_empty_slot_and_busy_recovery();
    test_event_errors_and_schedule_failure();

    if (failures != 0) {
        fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return 1;
    }

    puts("runtime macro ASCII/executor tests: PASS");
    return 0;
}
