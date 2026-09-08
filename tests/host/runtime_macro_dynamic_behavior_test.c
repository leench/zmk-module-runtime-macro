/*
 * Host-side tests for the dynamic runtime macro behavior forwarding semantics.
 *
 * The production behavior source is included directly so callback return values
 * can be tested without a devicetree-generated device table.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CONFIG_ZMK_LOG_LEVEL 0
#define POST_KERNEL 0
#define CONFIG_KERNEL_INIT_PRIORITY_DEFAULT 0
#define DT_HAS_COMPAT_STATUS_OKAY(...) 1
#define DT_INST_FOREACH_STATUS_OKAY(function)

static int dynamic_execute_result;
static unsigned int dynamic_execute_calls;
static bool dynamic_has_text;
static bool dynamic_text_consumed;

#include "../src/behaviors/behavior_runtime_macro_dynamic.c"

int zmk_runtime_macro_dynamic_execute(void) {
    dynamic_execute_calls++;
    if (dynamic_execute_result == 0 && dynamic_has_text) {
        dynamic_has_text = false;
        dynamic_text_consumed = true;
    }
    return dynamic_execute_result;
}

static int failures;

#define EXPECT_TRUE(condition)                                                                    \
    do {                                                                                          \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
            failures++;                                                                           \
        }                                                                                         \
    } while (false)

#define EXPECT_EQ(expected, actual)                                                               \
    do {                                                                                          \
        long long expected_value = (long long)(expected);                                         \
        long long actual_value = (long long)(actual);                                             \
        if (expected_value != actual_value) {                                                     \
            fprintf(stderr, "FAIL: %s:%d: expected %lld, got %lld\n", __FILE__, __LINE__,       \
                    expected_value, actual_value);                                                \
            failures++;                                                                           \
        }                                                                                         \
    } while (false)

static int invoke_pressed(void) {
    struct zmk_behavior_binding binding = {0};
    struct zmk_behavior_binding_event event = {
        .position = 7U,
    };

    return behavior_runtime_macro_dynamic_driver_api.binding_pressed(&binding, event);
}

static int invoke_released(void) {
    struct zmk_behavior_binding binding = {0};
    struct zmk_behavior_binding_event event = {
        .position = 7U,
    };

    return behavior_runtime_macro_dynamic_driver_api.binding_released(&binding, event);
}

static void reset_test_state(void) {
    dynamic_execute_result = 0;
    dynamic_execute_calls = 0U;
    dynamic_has_text = true;
    dynamic_text_consumed = false;
}

static void test_accepted_and_empty_are_opaque(void) {
    reset_test_state();

    EXPECT_EQ(ZMK_BEHAVIOR_OPAQUE, invoke_pressed());
    EXPECT_EQ(1U, dynamic_execute_calls);
    EXPECT_TRUE(!dynamic_has_text);
    EXPECT_TRUE(dynamic_text_consumed);

    reset_test_state();
    dynamic_has_text = false;

    EXPECT_EQ(ZMK_BEHAVIOR_OPAQUE, invoke_pressed());
    EXPECT_EQ(1U, dynamic_execute_calls);
    EXPECT_TRUE(!dynamic_has_text);
    EXPECT_TRUE(!dynamic_text_consumed);
}

static void test_busy_preserves_store_and_forwards_error(void) {
    reset_test_state();
    dynamic_execute_result = -EBUSY;

    EXPECT_EQ(-EBUSY, invoke_pressed());
    EXPECT_EQ(1U, dynamic_execute_calls);
    EXPECT_TRUE(dynamic_has_text);
}

static void test_start_error_is_forwarded(void) {
    reset_test_state();
    dynamic_execute_result = -EIO;

    EXPECT_EQ(-EIO, invoke_pressed());
    EXPECT_EQ(1U, dynamic_execute_calls);
    EXPECT_TRUE(dynamic_has_text);
}

static void test_release_is_opaque_without_execute(void) {
    reset_test_state();

    EXPECT_EQ(ZMK_BEHAVIOR_OPAQUE, invoke_released());
    EXPECT_EQ(0U, dynamic_execute_calls);
    EXPECT_TRUE(dynamic_has_text);
}

int main(void) {
    EXPECT_EQ(BEHAVIOR_LOCALITY_CENTRAL, behavior_runtime_macro_dynamic_driver_api.locality);
    test_accepted_and_empty_are_opaque();
    test_busy_preserves_store_and_forwards_error();
    test_start_error_is_forwarded();
    test_release_is_opaque_without_execute();

    if (failures != 0) {
        fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return 1;
    }

    puts("runtime macro dynamic behavior tests: PASS");
    return 0;
}
